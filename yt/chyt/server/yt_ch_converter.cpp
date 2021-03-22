#include "yt_ch_converter.h"

#include "config.h"
#include "columnar_conversion.h"
#include "data_type_boolean.h"

#include <yt/yt/client/table_client/helpers.h>
#include <yt/yt/client/table_client/logical_type.h>

#include <yt/yt/core/yson/pull_parser.h>
#include <yt/yt/core/yson/writer.h>
#include <yt/yt/core/yson/token_writer.h>

#include <Core/Types.h>
#include <Columns/IColumn.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnNothing.h>
#include <Columns/ColumnTuple.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>

#include <library/cpp/iterator/functools.h>

namespace NYT::NClickHouseServer {

using namespace NTableClient;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

template <typename... Args>
[[noreturn]] void ThrowConversionError(const TComplexTypeFieldDescriptor& descriptor, const Args&... args)
{
    THROW_ERROR_EXCEPTION(
        "Error converting %Qv of type %v to ClickHouse",
        descriptor.GetDescription(),
        *descriptor.GetType())
            << TError(args...);
}

//! Perform assignment column = newColumn also checking that new column is similar
//! to the original one in terms of data types. This helper is useful when conversion
//! deals with native YT columns, in which case columnar conversion methods create columns
//! by their own. We want to make sure that no type mismatch happens.
template <class TMutableColumnPtr>
void ReplaceColumnTypeChecked(TMutableColumnPtr& column, TMutableColumnPtr newColumn)
{
    YT_VERIFY(column);
    YT_VERIFY(column->structureEquals(*column));
    column.swap(newColumn);
}

////////////////////////////////////////////////////////////////////////////////

// Anonymous namespace prevents ODR violation between CH->YT and YT->CH internal
// implementation classes.
namespace {

////////////////////////////////////////////////////////////////////////////////

//! Node in the conversion tree-like structure. Child nodes are saved by
//! std::unique_ptr<IConverter> in member fields of particular implementations.
struct IConverter
{
    //! Consume single value expressed by YSON stream.
    virtual void ConsumeYson(TYsonPullParserCursor* cursor) = 0;
    //! Consume a batch of values represented by unversioned values.
    virtual void ConsumeUnversionedValues(TRange<TUnversionedValue> values) = 0;
    //! Consume given number of nulls.
    virtual void ConsumeNulls(int count) = 0;
    //! Consume native YT column.
    virtual void ConsumeYtColumn(const NTableClient::IUnversionedColumnarRowBatch::TColumn& column) = 0;

    virtual DB::ColumnPtr FlushColumn() = 0;
    virtual DB::DataTypePtr GetDataType() const = 0;
    virtual ~IConverter() = default;
};

using IConverterPtr = std::unique_ptr<IConverter>;

////////////////////////////////////////////////////////////////////////////////

//! This base implements ConsumeUnversionedValues and ConsumeYtColumn by assuming
//! that column consists of YSON strings and passing them to ConsumeYson (or ConsumeNull).
//!
//! Prerequisites:
//! - any value passed to ConsumeUnversionedValues should be Null, Any or Composite;
//! - any column passed to ConsumeYtColumn should be a string column containing valid YSONs.
class TYsonExtractingConverterBase
    : public IConverter
{
public:
    virtual void ConsumeUnversionedValues(TRange<TUnversionedValue> values) override
    {
        // NB: ConsumeYson leads to at least one virtual call per-value, so iterating
        // over all unversioned values is justified here.
        for (const auto& value : values) {
            YT_VERIFY(DispatchUnversionedValue(value));
        }
    }

    virtual void ConsumeYtColumn(const IUnversionedColumnarRowBatch::TColumn& column)
    {
        // TODO(max42): this may be done without full column materialization.

        auto stringColumn = ConvertStringLikeYTColumnToCHColumn(column);
        for (int index = 0; index < stringColumn->size(); ++index) {
            auto data = static_cast<std::string_view>(stringColumn->getDataAt(index));
            if (data.size() == 0) {
                ConsumeNulls(1);
            } else {
                TMemoryInput in(data);
                TYsonPullParser parser(&in, EYsonType::Node);
                TYsonPullParserCursor cursor(&parser);
                ConsumeYson(&cursor);
            }
        }
    }

private:
    bool DispatchUnversionedValue(TUnversionedValue value)
    {
        switch (value.Type) {
            case EValueType::Null:
                ConsumeNulls(1);
                return true;
            case EValueType::Any:
            case EValueType::Composite: {
                TMemoryInput in(value.Data.String, value.Length);
                TYsonPullParser parser(&in, EYsonType::Node);
                TYsonPullParserCursor cursor(&parser);
                ConsumeYson(&cursor);
                return true;
            }
            default:
                return false;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TRawYsonToStringConverter
    : public TYsonExtractingConverterBase
{
public:
    TRawYsonToStringConverter(const TComplexTypeFieldDescriptor& /* descriptor */, const TCompositeSettingsPtr& settings)
        : Column_(DB::ColumnString::create())
        , Settings_(settings)
        , YsonOutput_(YsonBuffer_)
        , YsonWriter_(&YsonOutput_, settings->DefaultYsonFormat)
    { }

    virtual void ConsumeYson(TYsonPullParserCursor* cursor)
    {
        cursor->TransferComplexValue(&YsonWriter_);

        PushValueFromWriter();
    }

    virtual void ConsumeNulls(int count)
    {
        // If somebody called ConsumeNulls() here, we are probably inside Nullable
        // column, so the exact value here does not matter.
        Column_->insertManyDefaults(count);
    }

    virtual void ConsumeUnversionedValues(TRange<TUnversionedValue> values) override
    {
        for (const auto& value : values) {
            UnversionedValueToYson(value, &YsonWriter_);
            PushValueFromWriter();
        }
    }

    virtual DB::ColumnPtr FlushColumn() override
    {
        return std::move(Column_);
    }

    virtual DB::DataTypePtr GetDataType() const override
    {
        return std::make_shared<DB::DataTypeString>();
    }

    virtual void ConsumeYtColumn(const IUnversionedColumnarRowBatch::TColumn& column)
    {
        // This is the outermost converter.
        // Input column may be of concrete type in case of any upcast, so we may need to
        // perform additional serialization to YSON.

        auto v1Type = CastToV1Type(column.Type).first;

        // TODO(max42): it is possible to eliminate intermediate column at all,
        // but I am too lazy to rewrite babenko@'s code at this moment.
        DB::MutableColumnPtr intermediateColumn;
        switch (v1Type) {
            case ESimpleLogicalValueType::Any:
                TYsonExtractingConverterBase::ConsumeYtColumn(column);
                return;
            case ESimpleLogicalValueType::String:
            case ESimpleLogicalValueType::Utf8:
            case ESimpleLogicalValueType::Json:
                intermediateColumn = ConvertStringLikeYTColumnToCHColumn(column);
                break;
            case ESimpleLogicalValueType::Int8:
            case ESimpleLogicalValueType::Int16:
            case ESimpleLogicalValueType::Int32:
            case ESimpleLogicalValueType::Int64:
            case ESimpleLogicalValueType::Uint8:
            case ESimpleLogicalValueType::Uint16:
            case ESimpleLogicalValueType::Uint32:
            case ESimpleLogicalValueType::Uint64:
            case ESimpleLogicalValueType::Date:
            case ESimpleLogicalValueType::Datetime:
            case ESimpleLogicalValueType::Timestamp:
            case ESimpleLogicalValueType::Interval:
                intermediateColumn = ConvertIntegerYTColumnToCHColumn(column, v1Type);
                break;
            case ESimpleLogicalValueType::Boolean:
                intermediateColumn = ConvertBooleanYTColumnToCHColumn(column);
                break;
            case ESimpleLogicalValueType::Double:
                intermediateColumn = ConvertDoubleYTColumnToCHColumn(column);
                break;
            case ESimpleLogicalValueType::Float:
                intermediateColumn = ConvertFloatYTColumnToCHColumn(column);
                break;
            default:
                // TODO(max42)
                YT_UNIMPLEMENTED();
        }

        ReplaceColumnTypeChecked(Column_, ConvertCHColumnToAny(
            *intermediateColumn,
            v1Type,
            Settings_->DefaultYsonFormat));
    }

private:
    DB::ColumnString::MutablePtr Column_;
    TCompositeSettingsPtr Settings_;
    TString YsonBuffer_;
    TStringOutput YsonOutput_;
    TYsonWriter YsonWriter_;

    void PushValueFromWriter()
    {
        auto& offsets = Column_->getOffsets();
        auto& chars = Column_->getChars();

        YsonWriter_.Flush();
        chars.insert(chars.end(), YsonBuffer_.begin(), YsonBuffer_.end());
        chars.push_back('\x0');
        offsets.push_back(chars.size());
        YsonBuffer_.clear();
    }
};

////////////////////////////////////////////////////////////////////////////////

template <ESimpleLogicalValueType LogicalType, class TCppType, class TColumn>
class TSimpleValueConverter
    : public IConverter
{
public:
    TSimpleValueConverter(TComplexTypeFieldDescriptor descriptor, DB::DataTypePtr dataType)
        : Descriptor_(std::move(descriptor))
        , DataType_(std::move(dataType))
        , Column_(DataType_->createColumn())
    { }

    virtual void ConsumeYson(TYsonPullParserCursor* cursor) override
    {
        auto ysonItem = cursor->GetCurrent();

        if constexpr (
            LogicalType == ESimpleLogicalValueType::Int8 ||
            LogicalType == ESimpleLogicalValueType::Int16 ||
            LogicalType == ESimpleLogicalValueType::Int32 ||
            LogicalType == ESimpleLogicalValueType::Int64 ||
            LogicalType == ESimpleLogicalValueType::Interval)
        {
            AssumeVectorColumn()->insertValue(ysonItem.UncheckedAsInt64());
        } else if constexpr (
            LogicalType == ESimpleLogicalValueType::Uint8 ||
            LogicalType == ESimpleLogicalValueType::Uint16 ||
            LogicalType == ESimpleLogicalValueType::Uint32 ||
            LogicalType == ESimpleLogicalValueType::Uint64 ||
            LogicalType == ESimpleLogicalValueType::Date ||
            LogicalType == ESimpleLogicalValueType::Datetime ||
            LogicalType == ESimpleLogicalValueType::Timestamp)
        {
            AssumeVectorColumn()->insertValue(ysonItem.UncheckedAsUint64());
        } else if constexpr (
            LogicalType == ESimpleLogicalValueType::Float ||
            LogicalType == ESimpleLogicalValueType::Double)
        {
            AssumeVectorColumn()->insertValue(ysonItem.UncheckedAsDouble());
        } else if constexpr (LogicalType == ESimpleLogicalValueType::Boolean) {
            AssumeVectorColumn()->insertValue(ysonItem.UncheckedAsBoolean());
        } else if constexpr (
            LogicalType == ESimpleLogicalValueType::String ||
            LogicalType == ESimpleLogicalValueType::Utf8)
        {
            auto data = ysonItem.UncheckedAsString();
            AssumeStringColumn()->insertData(data.data(), data.size());
        } else if constexpr (LogicalType == ESimpleLogicalValueType::Void) {
            YT_VERIFY(ysonItem.GetType() == EYsonItemType::EntityValue);
            AssumeNothingColumn()->insertDefault();
        } else {
            YT_ABORT();
        }
        cursor->Next();
    }

    virtual void ConsumeUnversionedValues(TRange<TUnversionedValue> values) override
    {
        for (const auto& value : values) {
            if (value.Type == EValueType::Null) {
                ConsumeNulls(1);
            } else {
                constexpr auto physicalType = GetPhysicalType(LogicalType);
                YT_VERIFY(value.Type == physicalType);

                if constexpr (
                    LogicalType == ESimpleLogicalValueType::Int8 ||
                    LogicalType == ESimpleLogicalValueType::Int16 ||
                    LogicalType == ESimpleLogicalValueType::Int32 ||
                    LogicalType == ESimpleLogicalValueType::Int64 ||
                    LogicalType == ESimpleLogicalValueType::Interval)
                {
                    AssumeVectorColumn()->insertValue(value.Data.Int64);
                } else if constexpr (
                    LogicalType == ESimpleLogicalValueType::Uint8 ||
                    LogicalType == ESimpleLogicalValueType::Uint16 ||
                    LogicalType == ESimpleLogicalValueType::Uint32 ||
                    LogicalType == ESimpleLogicalValueType::Uint64 ||
                    LogicalType == ESimpleLogicalValueType::Date ||
                    LogicalType == ESimpleLogicalValueType::Datetime ||
                    LogicalType == ESimpleLogicalValueType::Timestamp)
                {
                    AssumeVectorColumn()->insertValue(value.Data.Uint64);
                } else if constexpr (
                    LogicalType == ESimpleLogicalValueType::Float ||
                    LogicalType == ESimpleLogicalValueType::Double)
                {
                    AssumeVectorColumn()->insertValue(value.Data.Double);
                } else if constexpr (LogicalType == ESimpleLogicalValueType::Boolean) {
                    AssumeVectorColumn()->insertValue(value.Data.Boolean);
                } else if constexpr (
                    LogicalType == ESimpleLogicalValueType::String ||
                    LogicalType == ESimpleLogicalValueType::Utf8)
                {
                    AssumeStringColumn()->insertData(value.Data.String, value.Length);
                } else if constexpr (LogicalType == ESimpleLogicalValueType::Void) {
                    AssumeNothingColumn()->insertDefault();
                } else {
                    YT_ABORT();
                }
            }
        }
    }

    virtual void ConsumeYtColumn(const IUnversionedColumnarRowBatch::TColumn& column) override
    {
        if constexpr (
            LogicalType == ESimpleLogicalValueType::Int8 ||
            LogicalType == ESimpleLogicalValueType::Int16 ||
            LogicalType == ESimpleLogicalValueType::Int32 ||
            LogicalType == ESimpleLogicalValueType::Int64 ||
            LogicalType == ESimpleLogicalValueType::Interval ||
            LogicalType == ESimpleLogicalValueType::Uint8 ||
            LogicalType == ESimpleLogicalValueType::Uint16 ||
            LogicalType == ESimpleLogicalValueType::Uint32 ||
            LogicalType == ESimpleLogicalValueType::Uint64 ||
            LogicalType == ESimpleLogicalValueType::Date ||
            LogicalType == ESimpleLogicalValueType::Datetime ||
            LogicalType == ESimpleLogicalValueType::Timestamp)
        {
            ReplaceColumnTypeChecked(Column_, ConvertIntegerYTColumnToCHColumn(column, LogicalType));
        } else if constexpr (LogicalType == ESimpleLogicalValueType::Float) {
            ReplaceColumnTypeChecked(Column_, ConvertFloatYTColumnToCHColumn(column));
        } else if constexpr (LogicalType == ESimpleLogicalValueType::Double) {
            ReplaceColumnTypeChecked(Column_, ConvertDoubleYTColumnToCHColumn(column));
        } else if constexpr (LogicalType == ESimpleLogicalValueType::Boolean) {
            ReplaceColumnTypeChecked(Column_, ConvertBooleanYTColumnToCHColumn(column));
        } else if constexpr (
            LogicalType == ESimpleLogicalValueType::String ||
            LogicalType == ESimpleLogicalValueType::Utf8)
        {
            ReplaceColumnTypeChecked(Column_, ConvertStringLikeYTColumnToCHColumn(column));
        } else if constexpr (LogicalType == ESimpleLogicalValueType::Void) {
            // AssumeNothingColumn()->insertDefault(column);
        } else {
            YT_ABORT();
        }
    }

    virtual void ConsumeNulls(int count) override
    {
        // If somebody called ConsumeNulls() here, we are probably inside Nullable
        // column, so the exact value here does not matter.
        Column_->insertManyDefaults(count);
    }

    virtual DB::ColumnPtr FlushColumn() override
    {
        return std::move(Column_);
    }

    virtual DB::DataTypePtr GetDataType() const override
    {
        return DataType_;
    }

private:
    TComplexTypeFieldDescriptor Descriptor_;
    DB::DataTypePtr DataType_;
    DB::IColumn::MutablePtr Column_;

    DB::ColumnVector<TCppType>* AssumeVectorColumn()
    {
        return static_cast<DB::ColumnVector<TCppType>*>(Column_.get());
    }

    DB::ColumnString* AssumeStringColumn()
    {
        return static_cast<DB::ColumnString*>(Column_.get());
    }

    DB::ColumnNothing* AssumeNothingColumn()
    {
        return static_cast<DB::ColumnNothing*>(Column_.get());
    }

    void SetColumnChecked(DB::IColumn::MutablePtr column)
    {
        YT_VERIFY(Column_->structureEquals(*column));
        Column_.swap(column);
    }
};

////////////////////////////////////////////////////////////////////////////////

// NB: there is an important difference on how optional<T> works for outermost case with
// simple T (so called V1 optional scenario) and the rest of cases.
//
// For V1 optionals input unversioned values may be either of type T or of type Null.
// Input native YT columns will also be properly typed, i.e. input column will be of type T
// with null bitmap.
//
// For non-V1 optionals input unversioned values are always Any or Composite (shame on me,
// I still don't get the difference...). Similarly, input native YT columns will always be
// string columns. I am not sure if these string columns may provide non-trivial null bitmap,
// but that makes not much difference as our implementation is ready for that.
template <bool IsV1Optional>
class TOptionalConverter
    : public TYsonExtractingConverterBase
{
public:
    TOptionalConverter(IConverterPtr underlyingConverter, int nestingLevel)
        : UnderlyingConverter_(std::move(underlyingConverter))
        , NestingLevel_(nestingLevel)
    {
        // Tuples and arrays cannot be inside Nullable() in ClickHouse.
        // Also note that all non-simple types are represented as tuples and arrays.
        // Both DB::makeNullable are silently returning original argument if they see
        // something that is already nullable.
        if (UnderlyingConverter_->GetDataType()->canBeInsideNullable()) {
            NullColumn_ = DB::ColumnVector<DB::UInt8>::create();
        }
    }

    virtual void ConsumeYson(TYsonPullParserCursor* cursor) override
    {
        int outerOptionalsFound = 0;
        while (cursor->GetCurrent().GetType() == EYsonItemType::BeginList && outerOptionalsFound < NestingLevel_ - 1) {
            ++outerOptionalsFound;
            cursor->Next();
        }
        if (outerOptionalsFound < NestingLevel_ - 1) {
            // This have to be entity of some level.
            YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EntityValue);
            ConsumeNulls(1);
            cursor->Next();
        } else {
            YT_VERIFY(outerOptionalsFound == NestingLevel_ - 1);
            // It may either be entity or a representation of underlying non-optional type.
            if (cursor->GetCurrent().GetType() == EYsonItemType::EntityValue) {
                ConsumeNulls(1);
                cursor->Next();
            } else {
                if (NullColumn_) {
                    NullColumn_->insertValue(0);
                }
                UnderlyingConverter_->ConsumeYson(cursor);
            }
        }
        while (outerOptionalsFound--) {
            YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EndList);
            cursor->Next();
        }
    }

    virtual void ConsumeUnversionedValues(TRange<TUnversionedValue> values) override
    {
        if constexpr (IsV1Optional) {
            // V1 optional converter always faces either Null or underlying type.
            if (NullColumn_) {
                for (const auto& value : values) {
                    NullColumn_->insertValue(value.Type == EValueType::Null ? 1 : 0);
                }
            }
            UnderlyingConverter_->ConsumeUnversionedValues(values);
        } else {
            // Non-v1 optional always deals with Any/Composite. We may safely assert that.
            // Also, making a virtual call per value is OK in this case.
            TYsonExtractingConverterBase::ConsumeUnversionedValues(values);
        }
    }

    virtual void ConsumeNulls(int count) override
    {
        if (NullColumn_) {
            // TODO(max42): there is no efficient (in terms of virtual calls) way
            // of inserting same value many times to the column. Introduce it.
            for (int index = 0; index < count; ++index) {
                NullColumn_->insert(1);
            }
        }
        UnderlyingConverter_->ConsumeNulls(count);
    }

    virtual void ConsumeYtColumn(const IUnversionedColumnarRowBatch::TColumn& column)
    {
        if constexpr (IsV1Optional) {
            if (NullColumn_) {
                ReplaceColumnTypeChecked(NullColumn_, BuildNullBytemapForCHColumn(column));
            }

            UnderlyingConverter_->ConsumeYtColumn(column);
        } else {
            TYsonExtractingConverterBase::ConsumeYtColumn(column);
        }
    }

    virtual DB::ColumnPtr FlushColumn() override
    {
        auto underlyingColumn = UnderlyingConverter_->FlushColumn();

        if (NullColumn_) {
            // Pass ownership to ColumnNullable and make sure it won't be COWed.
            DB::ColumnVector<DB::UInt8>::Ptr nullColumn = std::move(NullColumn_);
            YT_VERIFY(nullColumn->use_count() == 1);
            return DB::ColumnNullable::create(underlyingColumn, nullColumn);
        } else {
            return underlyingColumn;
        }
    }

    virtual DB::DataTypePtr GetDataType() const override
    {
        if (UnderlyingConverter_->GetDataType()->canBeInsideNullable()) {
            return std::make_shared<DB::DataTypeNullable>(UnderlyingConverter_->GetDataType());
        } else {
            return UnderlyingConverter_->GetDataType();
        }
    }

private:
    const IConverterPtr UnderlyingConverter_;
    int NestingLevel_;
    DB::ColumnVector<DB::UInt8>::MutablePtr NullColumn_;
};

////////////////////////////////////////////////////////////////////////////////

class TListConverter
    : public TYsonExtractingConverterBase
{
public:
    TListConverter(IConverterPtr underlyingConverter)
        : UnderlyingConverter_(std::move(underlyingConverter))
        , ColumnOffsets_(DB::ColumnVector<ui64>::create())
    { }

    virtual void ConsumeYson(TYsonPullParserCursor* cursor) override
    {
        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::BeginList);
        cursor->Next();

        while (cursor->GetCurrent().GetType() != EYsonItemType::EndList && cursor->GetCurrent().GetType() != EYsonItemType::EndOfStream) {
            UnderlyingConverter_->ConsumeYson(cursor);
            ++ItemCount_;
        }

        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EndList);
        cursor->Next();

        ColumnOffsets_->insertValue(ItemCount_);
    }

    virtual void ConsumeNulls(int count) override
    {
        // Null is represented as an empty array.

        // TODO(max42): there is no efficient (in terms of virtual calls) way
        // of inserting same value many times to the column. Introduce it.
        for (int index = 0; index < count; ++index) {
            ColumnOffsets_->insertValue(ItemCount_);
        }
    }

    virtual DB::ColumnPtr FlushColumn() override
    {
        DB::ColumnVector<ui64>::Ptr columnOffsets = std::move(ColumnOffsets_);
        return DB::ColumnArray::create(UnderlyingConverter_->FlushColumn(), columnOffsets);
    }

    virtual DB::DataTypePtr GetDataType() const override
    {
        return std::make_shared<DB::DataTypeArray>(UnderlyingConverter_->GetDataType());
    }

private:
    IConverterPtr UnderlyingConverter_;
    DB::ColumnVector<ui64>::MutablePtr ColumnOffsets_;
    ui64 ItemCount_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TDictConverter
    : public TYsonExtractingConverterBase
{
public:
    TDictConverter(IConverterPtr keyConverter, IConverterPtr valueConverter)
        : KeyConverter_(std::move(keyConverter))
        , ValueConverter_(std::move(valueConverter))
        , ColumnOffsets_(DB::ColumnVector<ui64>::create())
    { }

    virtual void ConsumeYson(TYsonPullParserCursor* cursor) override
    {
        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::BeginList);
        cursor->Next();

        while (cursor->GetCurrent().GetType() != EYsonItemType::EndList && cursor->GetCurrent().GetType() != EYsonItemType::EndOfStream) {
            YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::BeginList);
            cursor->Next();
            KeyConverter_->ConsumeYson(cursor);
            ValueConverter_->ConsumeYson(cursor);
            YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EndList);
            cursor->Next();
            ++ItemCount_;
        }

        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EndList);
        cursor->Next();

        ColumnOffsets_->insertValue(ItemCount_);
    }

    virtual void ConsumeNulls(int count) override
    {
        // Null is represented as an empty array.

        // TODO(max42): there is no efficient (in terms of virtual calls) way
        // of inserting same value many times to the column. Introduce it.
        for (int index = 0; index < count; ++index) {
            ColumnOffsets_->insertValue(ItemCount_);
        }
    }

    virtual DB::ColumnPtr FlushColumn() override
    {
        DB::ColumnVector<ui64>::Ptr columnOffsets = std::move(ColumnOffsets_);
        auto keyColumn = KeyConverter_->FlushColumn()->assumeMutable();
        auto valueColumn = ValueConverter_->FlushColumn()->assumeMutable();
        std::vector<DB::IColumn::MutablePtr> columns;
        columns.emplace_back(std::move(keyColumn));
        columns.emplace_back(std::move(valueColumn));
        auto columnTuple = DB::ColumnTuple::create(std::move(columns));
        return DB::ColumnArray::create(std::move(columnTuple), columnOffsets);
    }

    virtual DB::DataTypePtr GetDataType() const override
    {
        auto tupleDataType = std::make_shared<DB::DataTypeTuple>(
            std::vector<DB::DataTypePtr>{KeyConverter_->GetDataType(), ValueConverter_->GetDataType()},
            std::vector<std::string>{"key", "value"});

        return std::make_shared<DB::DataTypeArray>(std::move(tupleDataType));
    }

private:
    IConverterPtr KeyConverter_;
    IConverterPtr ValueConverter_;
    DB::ColumnVector<ui64>::MutablePtr ColumnOffsets_;
    ui64 ItemCount_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TTupleConverter
    : public TYsonExtractingConverterBase
{
public:
    TTupleConverter(std::vector<IConverterPtr> itemConverters)
        : ItemConverters_(std::move(itemConverters))
    { }

    virtual void ConsumeYson(TYsonPullParserCursor* cursor) override
    {
        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::BeginList);
        cursor->Next();

        for (const auto& itemConverter : ItemConverters_) {
            itemConverter->ConsumeYson(cursor);
        }

        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EndList);
        cursor->Next();
    }

    virtual void ConsumeNulls(int count) override
    {
        // Null is represented as a tuple of defaults.
        for (const auto& itemConverter : ItemConverters_) {
            itemConverter->ConsumeNulls(count);
        }
    }

    virtual DB::ColumnPtr FlushColumn() override
    {
        std::vector<DB::IColumn::MutablePtr> underlyingColumns;

        for (const auto& itemConverter : ItemConverters_) {
            underlyingColumns.emplace_back(itemConverter->FlushColumn()->assumeMutable());
        }
        return DB::ColumnTuple::create(std::move(underlyingColumns));
    }

    virtual DB::DataTypePtr GetDataType() const override
    {
        std::vector<DB::DataTypePtr> dataTypes;
        for (const auto& itemConverter : ItemConverters_) {
            dataTypes.emplace_back(itemConverter->GetDataType());
        }
        return std::make_shared<DB::DataTypeTuple>(dataTypes);
    }

private:
    std::vector<IConverterPtr> ItemConverters_;
};

////////////////////////////////////////////////////////////////////////////////

class TStructConverter
    : public TYsonExtractingConverterBase
{
public:
    TStructConverter(std::vector<IConverterPtr> fieldConverters, std::vector<TString> fieldNames)
        : FieldConverters_(std::move(fieldConverters))
        , FieldNames_(std::move(fieldNames))
    {
        for (const auto& [index, fieldName] : Enumerate(FieldNames_)) {
            FieldNameToPosition_[fieldName] = index;
        }
    }

    virtual void ConsumeYson(TYsonPullParserCursor* cursor) override
    {
        if (cursor->GetCurrent().GetType() == EYsonItemType::BeginList) {
            ConsumePositional(cursor);
        } else if (cursor->GetCurrent().GetType() == EYsonItemType::BeginMap) {
            ConsumeNamed(cursor);
        } else {
            YT_ABORT();
        }
    }

    virtual void ConsumeNulls(int count) override
    {
        // Null is represented as a tuple of defaults.
        for (const auto& fieldConverter : FieldConverters_) {
            fieldConverter->ConsumeNulls(count);
        }
    }

    virtual DB::ColumnPtr FlushColumn() override
    {
        std::vector<DB::IColumn::MutablePtr> underlyingColumns;

        for (const auto& fieldConverter : FieldConverters_) {
            underlyingColumns.emplace_back(fieldConverter->FlushColumn()->assumeMutable());
        }
        return DB::ColumnTuple::create(std::move(underlyingColumns));
    }

    virtual DB::DataTypePtr GetDataType() const override
    {
        std::vector<DB::DataTypePtr> dataTypes;
        for (const auto& FieldConverter : FieldConverters_) {
            dataTypes.emplace_back(FieldConverter->GetDataType());
        }
        return std::make_shared<DB::DataTypeTuple>(dataTypes, std::vector<std::string>(FieldNames_.begin(), FieldNames_.end()));
    }

private:
    std::vector<IConverterPtr> FieldConverters_;
    std::vector<TString> FieldNames_;
    THashMap<TString, int> FieldNameToPosition_;

    void ConsumeNamed(TYsonPullParserCursor* cursor)
    {
        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::BeginMap);
        cursor->Next();
        std::vector<bool> seenPositions(FieldConverters_.size());
        while (cursor->GetCurrent().GetType() != EYsonItemType::EndMap) {
            auto key = cursor->GetCurrent().UncheckedAsString();
            auto position = GetOrCrash(FieldNameToPosition_, key);
            cursor->Next();
            YT_VERIFY(!seenPositions[position]);
            seenPositions[position] = true;
            FieldConverters_[position]->ConsumeYson(cursor);
        }
        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EndMap);
        cursor->Next();

        for (int index = 0; index < seenPositions.size(); ++index) {
            if (!seenPositions[index]) {
                FieldConverters_[index]->ConsumeNulls(1);
            }
        }
    }

    void ConsumePositional(TYsonPullParserCursor* cursor)
    {
        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::BeginList);
        cursor->Next();
        for (int index = 0; index < FieldConverters_.size(); ++index) {
            if (cursor->GetCurrent().GetType() == EYsonItemType::EndList) {
                FieldConverters_[index]->ConsumeNulls(1);
            } else {
                FieldConverters_[index]->ConsumeYson(cursor);
            }
        }
        YT_VERIFY(cursor->GetCurrent().GetType() == EYsonItemType::EndList);
        cursor->Next();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TYTCHConverter::TImpl
{
public:
    TImpl(TComplexTypeFieldDescriptor descriptor, TCompositeSettingsPtr settings, bool enableReadOnlyConversions)
        : Descriptor_(std::move(descriptor))
        , Settings_(std::move(settings))
        , EnableReadOnlyConversions_(enableReadOnlyConversions)
        , RootConverter_(CreateConverter(Descriptor_, /* isOutermost */ true))
    { }

    void ConsumeUnversionedValues(TRange<TUnversionedValue> values)
    {
        RootConverter_->ConsumeUnversionedValues(values);
    }

    void ConsumeYson(TYsonStringBuf yson)
    {
        TMemoryInput in(yson.AsStringBuf());
        TYsonPullParser parser(&in, EYsonType::Node);
        TYsonPullParserCursor cursor(&parser);
        RootConverter_->ConsumeYson(&cursor);
        YT_VERIFY(cursor->IsEndOfStream());
    }

    void ConsumeNulls(int count)
    {
        // This may result in either adding null or default value in case top-most type is not
        // enclosible in Nullable.
        RootConverter_->ConsumeNulls(count);
    }

    void ConsumeYtColumn(const NTableClient::IUnversionedColumnarRowBatch::TColumn& column)
    {
        RootConverter_->ConsumeYtColumn(column);
    }

    DB::ColumnPtr FlushColumn()
    {
        return RootConverter_->FlushColumn();
    }

    DB::DataTypePtr GetDataType() const
    {
        return RootConverter_->GetDataType();
    }

private:
    TComplexTypeFieldDescriptor Descriptor_;
    TCompositeSettingsPtr Settings_;
    bool EnableReadOnlyConversions_;

    IConverterPtr RootConverter_;

    void ValidateReadOnly(const TComplexTypeFieldDescriptor& descriptor)
    {
        if (!EnableReadOnlyConversions_) {
            THROW_ERROR_EXCEPTION(
                "Field %Qv has type %v which is supported only for reading",
                descriptor.GetDescription(),
                *descriptor.GetType());
        }
    }

    IConverterPtr CreateSimpleLogicalTypeConverter(ESimpleLogicalValueType valueType, const TComplexTypeFieldDescriptor& descriptor)
    {
        IConverterPtr converter;
        switch (valueType) {
            #define CASE(caseValueType, TCppType, TColumn, dataType)                                       \
                case caseValueType:                                                                        \
                    converter = std::make_unique<TSimpleValueConverter<caseValueType, TCppType, TColumn>>( \
                        descriptor,                                                                        \
                        dataType);                                                                         \
                    break;

            #define CASE_NUMERIC(caseValueType, TCppType) CASE(caseValueType, TCppType, DB::ColumnVector<TCppType>, std::make_shared<DB::DataTypeNumber<TCppType>>())

            CASE_NUMERIC(ESimpleLogicalValueType::Uint8, DB::UInt8)
            CASE_NUMERIC(ESimpleLogicalValueType::Uint16, ui16)
            CASE_NUMERIC(ESimpleLogicalValueType::Uint32, ui32)
            CASE_NUMERIC(ESimpleLogicalValueType::Uint64, ui64)
            CASE_NUMERIC(ESimpleLogicalValueType::Int8, i8)
            CASE_NUMERIC(ESimpleLogicalValueType::Int16, i16)
            CASE_NUMERIC(ESimpleLogicalValueType::Int32, i32)
            CASE_NUMERIC(ESimpleLogicalValueType::Int64, i64)
            CASE_NUMERIC(ESimpleLogicalValueType::Float, float)
            CASE_NUMERIC(ESimpleLogicalValueType::Double, double)
            CASE_NUMERIC(ESimpleLogicalValueType::Interval, i64)
            CASE_NUMERIC(ESimpleLogicalValueType::Timestamp, ui64)
            CASE(ESimpleLogicalValueType::Boolean, DB::UInt8, DB::ColumnVector<DB::UInt8>, GetDataTypeBoolean())
            // TODO(max42): specify timezone explicitly here.
            CASE(ESimpleLogicalValueType::Date, ui16, DB::ColumnVector<ui16>, std::make_shared<DB::DataTypeDate>())
            CASE(ESimpleLogicalValueType::Datetime, ui32, DB::ColumnVector<ui32>, std::make_shared<DB::DataTypeDateTime>())
            CASE(ESimpleLogicalValueType::String, DB::UInt8 /* actually unused */, DB::ColumnString, std::make_shared<DB::DataTypeString>())
            CASE(ESimpleLogicalValueType::Utf8, DB::UInt8 /* actually unused */, DB::ColumnString, std::make_shared<DB::DataTypeString>())
            CASE(ESimpleLogicalValueType::Void, DB::UInt8 /* actually unused */, DB::ColumnNothing, std::make_shared<DB::DataTypeNothing>())
            default:
                ThrowConversionError(descriptor, "Converting YT simple logical value type %v to ClickHouse is not supported", valueType);
        }
        return converter;
    }

    IConverterPtr CreateOptionalConverter(const TComplexTypeFieldDescriptor& descriptor, bool isOutermost)
    {
        // Descend to first non-optional enclosed type.
        auto nonOptionalDescriptor = descriptor;
        int nestingLevel = 0;
        while (nonOptionalDescriptor.GetType()->IsNullable()) {
            nonOptionalDescriptor = nonOptionalDescriptor.OptionalElement();
            ++nestingLevel;
        }

        YT_VERIFY(nestingLevel > 0);

        bool isV1Optional = isOutermost && (nestingLevel == 1) &&
            nonOptionalDescriptor.GetType()->GetMetatype() == ELogicalMetatype::Simple;

        auto underlyingConverter = CreateConverter(nonOptionalDescriptor);

        if (!underlyingConverter->GetDataType()->canBeInsideNullable() || nestingLevel >= 2) {
            ValidateReadOnly(descriptor);
        }

        if (isV1Optional) {
            return std::make_unique<TOptionalConverter<true>>(std::move(underlyingConverter), nestingLevel);
        } else {
            return std::make_unique<TOptionalConverter<false>>(std::move(underlyingConverter), nestingLevel);
        }

    }

    IConverterPtr CreateListConverter(const TComplexTypeFieldDescriptor& descriptor)
    {
        auto underlyingConverter = CreateConverter(descriptor.ListElement());

        return std::make_unique<TListConverter>(std::move(underlyingConverter));
    }

    IConverterPtr CreateDictConverter(const TComplexTypeFieldDescriptor& descriptor)
    {
        auto keyConverter = CreateConverter(descriptor.DictKey());
        auto valueConverter = CreateConverter(descriptor.DictValue());

        return std::make_unique<TDictConverter>(std::move(keyConverter), std::move(valueConverter));
    }

    IConverterPtr CreateTupleConverter(const TComplexTypeFieldDescriptor& descriptor)
    {
        auto tupleLength = descriptor.GetType()->AsTupleTypeRef().GetElements().size();
        std::vector<IConverterPtr> itemConverters;
        for (int index = 0; index < tupleLength; ++index) {
            itemConverters.emplace_back(CreateConverter(descriptor.TupleElement(index)));
        }

        return std::make_unique<TTupleConverter>(std::move(itemConverters));
    }

    IConverterPtr CreateStructConverter(const TComplexTypeFieldDescriptor& descriptor)
    {
        auto structLength = descriptor.GetType()->AsStructTypeRef().GetFields().size();
        std::vector<IConverterPtr> fieldConverters;
        std::vector<TString> fieldNames;
        for (const auto& structField : descriptor.GetType()->AsStructTypeRef().GetFields()) {
            fieldNames.emplace_back(structField.Name);
        }
        for (int index = 0; index < structLength; ++index) {
            fieldConverters.emplace_back(CreateConverter(descriptor.StructField(index)));
        }

        return std::make_unique<TStructConverter>(std::move(fieldConverters), std::move(fieldNames));
    }

    IConverterPtr CreateConverter(const TComplexTypeFieldDescriptor& descriptor, bool isOutermost = false)
    {
        const auto& type = descriptor.GetType();
        if (type->GetMetatype() == ELogicalMetatype::Simple) {
            const auto& simpleType = type->AsSimpleTypeRef();
            if (simpleType.GetElement() == ESimpleLogicalValueType::Any ||
                simpleType.GetElement() == ESimpleLogicalValueType::Null ||
                simpleType.GetElement() == ESimpleLogicalValueType::Void)
            {
                return std::make_unique<TRawYsonToStringConverter>(descriptor, Settings_);
            } else {
                return CreateSimpleLogicalTypeConverter(simpleType.GetElement(), descriptor);
            }
        } else if (type->GetMetatype() == ELogicalMetatype::Optional) {
            return CreateOptionalConverter(descriptor, isOutermost);
        } else if (type->GetMetatype() == ELogicalMetatype::List) {
            return CreateListConverter(descriptor);
        } else if (type->GetMetatype() == ELogicalMetatype::Dict) {
            ValidateReadOnly(descriptor);
            return CreateDictConverter(descriptor);
        } else if (type->GetMetatype() == ELogicalMetatype::Tuple) {
            return CreateTupleConverter(descriptor);
        } else if (type->GetMetatype() == ELogicalMetatype::Struct) {
            return CreateStructConverter(descriptor);
        } else {
            ValidateReadOnly(descriptor);
            // Perform fallback to raw yson.
            return std::make_unique<TRawYsonToStringConverter>(descriptor, Settings_);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TYTCHConverter::TYTCHConverter(
    TComplexTypeFieldDescriptor descriptor,
    TCompositeSettingsPtr settings,
    bool enableReadOnlyConversions)
    : Impl_(std::make_unique<TImpl>(std::move(descriptor), std::move(settings), enableReadOnlyConversions))
{ }

void TYTCHConverter::ConsumeUnversionedValues(TRange<TUnversionedValue> values)
{
    return Impl_->ConsumeUnversionedValues(values);
}

void TYTCHConverter::ConsumeYson(TYsonStringBuf yson)
{
    return Impl_->ConsumeYson(yson);
}

DB::ColumnPtr TYTCHConverter::FlushColumn()
{
    return Impl_->FlushColumn();
}

DB::DataTypePtr TYTCHConverter::GetDataType() const
{
    return Impl_->GetDataType();
}

void TYTCHConverter::ConsumeNulls(int count)
{
    return Impl_->ConsumeNulls(count);
}

void TYTCHConverter::ConsumeYtColumn(
    const NTableClient::IUnversionedColumnarRowBatch::TColumn& column)
{
    return Impl_->ConsumeYtColumn(column);
}

TYTCHConverter::~TYTCHConverter() = default;

TYTCHConverter::TYTCHConverter(
    TYTCHConverter&& other)
    : Impl_(std::move(other.Impl_))
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer