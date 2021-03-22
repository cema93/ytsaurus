#include "block_input_stream.h"

#include "query_context.h"
#include "host.h"
#include "helpers.h"
#include "config.h"
#include "subquery_spec.h"
#include "conversion.h"

#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/parallel_reader_memory_manager.h>

#include <yt/yt/ytlib/table_client/virtual_value_directory.h>

#include <yt/yt/client/table_client/row_batch.h>
#include <yt/yt/client/table_client/name_table.h>

#include <yt/yt/core/ytree/yson_serializable.h>

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnVector.h>

#include <DataTypes/DataTypeNothing.h>

#include <Interpreters/ExpressionActions.h>

namespace NYT::NClickHouseServer {

using namespace NTableClient;
using namespace NLogging;
using namespace NConcurrency;
using namespace NTracing;
using namespace NChunkClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

namespace {

TTableSchemaPtr InsertVirtualColumns(
    const TTableSchemaPtr& schema,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TString>& virtualColumnNames)
{
    std::vector<TColumnSchema> columns = schema->Columns();

    if (!dataSourceDirectory->DataSources().empty()) {
        const auto& virtualValueDirectory = dataSourceDirectory->DataSources()[0].GetVirtualValueDirectory();

        // All virtual value directory should share same schema.
        for (const auto& dataSource : dataSourceDirectory->DataSources()) {
            if (virtualValueDirectory) {
                YT_VERIFY(dataSource.GetVirtualValueDirectory());
                YT_VERIFY(*dataSource.GetVirtualValueDirectory()->Schema == *virtualValueDirectory->Schema);
            } else {
                YT_VERIFY(!dataSource.GetVirtualValueDirectory());
            }
        }

        if (virtualValueDirectory) {
            const auto virtualColumns = virtualValueDirectory->Schema->Filter(virtualColumnNames)->Columns();
            columns.insert(columns.end(), virtualColumns.begin(), virtualColumns.end());
        }
    }

    return New<TTableSchema>(
        std::move(columns),
        schema->GetStrict(),
        schema->GetUniqueKeys(),
        schema->GetSchemaModification());
}

TClientBlockReadOptions CreateBlockReadOptions(const TString& user)
{
    TClientBlockReadOptions blockReadOptions;
    blockReadOptions.ChunkReaderStatistics = New<TChunkReaderStatistics>();
    blockReadOptions.WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::UserRealtime);
    blockReadOptions.WorkloadDescriptor.CompressionFairShareTag = user;
    blockReadOptions.ReadSessionId = NChunkClient::TReadSessionId::Create();
    return blockReadOptions;
}

// Analog of the method from MergeTreeBaseSelectBlockInputStream::executePrewhereActions from CH.
void ExecutePrewhereActions(DB::Block& block, const DB::PrewhereInfoPtr& prewhereInfo)
{
    if (prewhereInfo->alias_actions) {
        prewhereInfo->alias_actions->execute(block);
    }
    prewhereInfo->prewhere_actions->execute(block);
    if (!block) {
        block.insert({nullptr, std::make_shared<DB::DataTypeNothing>(), "_nothing"});
    }
}

DB::Block FilterRowsByPrewhereInfo(
    DB::Block&& blockToFilter,
    const DB::PrewhereInfoPtr& prewhereInfo)
{
    auto columnsWithTypeAndName = blockToFilter.getColumnsWithTypeAndName();

    // Create prewhere column for filtering.
    ExecutePrewhereActions(blockToFilter, prewhereInfo);

    // Extract or materialize filter data.
    // Note that prewhere column is either UInt8 or Nullable(UInt8).
    const DB::IColumn::Filter* filter;
    DB::IColumn::Filter materializedFilter;
    const auto& prewhereColumn = blockToFilter.getByName(prewhereInfo->prewhere_column_name).column;
    if (const auto* nullablePrewhereColumn = DB::checkAndGetColumn<DB::ColumnNullable>(prewhereColumn.get())) {
        const auto* prewhereNullsColumn = DB::checkAndGetColumn<DB::ColumnVector<DB::UInt8>>(nullablePrewhereColumn->getNullMapColumn());
        YT_VERIFY(prewhereNullsColumn);
        const auto& prewhereNulls = prewhereNullsColumn->getData();

        const auto* prewhereValuesColumn = DB::checkAndGetColumn<DB::ColumnVector<DB::UInt8>>(nullablePrewhereColumn->getNestedColumn());
        YT_VERIFY(prewhereValuesColumn);
        const auto& prewhereValues = prewhereValuesColumn->getData();

        YT_VERIFY(prewhereNulls.size() == prewhereValues.size());
        auto rowCount = prewhereValues.size();
        materializedFilter.resize_exact(rowCount);
        for (size_t index = 0; index < rowCount; ++index) {
            materializedFilter[index] = static_cast<ui8>(prewhereNulls[index] == 0 && prewhereValues[index] != 0);
        }
        filter = &materializedFilter;
    } else {
        const auto* boolPrewhereColumn = DB::checkAndGetColumn<DB::ColumnVector<DB::UInt8>>(prewhereColumn.get());
        YT_VERIFY(boolPrewhereColumn);
        filter = &boolPrewhereColumn->getData();
    }

    // Apply filter.
    for (auto& columnWithTypeAndName : columnsWithTypeAndName) {
        columnWithTypeAndName.column = columnWithTypeAndName.column->filter(*filter, 0);
    }
    auto filteredBlock = DB::Block(std::move(columnsWithTypeAndName));

    // Execute prewhere actions for filtered block.
    ExecutePrewhereActions(filteredBlock, prewhereInfo);

    return filteredBlock;
}

}  // namespace

TBlockInputStream::TBlockInputStream(
    ISchemalessMultiChunkReaderPtr reader,
    TTableSchemaPtr readSchemaWithVirtualColumns,
    TTraceContextPtr traceContext,
    THost* host,
    TQuerySettingsPtr settings,
    TLogger logger,
    DB::PrewhereInfoPtr prewhereInfo)
    : Reader_(std::move(reader))
    , ReadSchemaWithVirtualColumns_(std::move(readSchemaWithVirtualColumns))
    , TraceContext_(std::move(traceContext))
    , Host_(host)
    , Settings_(std::move(settings))
    , Logger(std::move(logger))
    , RowBuffer_(New<NTableClient::TRowBuffer>())
    , PrewhereInfo_(std::move(prewhereInfo))
{
    Prepare();
}

std::string TBlockInputStream::getName() const
{
    return "BlockInputStream";
}

DB::Block TBlockInputStream::getHeader() const
{
    return OutputHeaderBlock_;
}

void TBlockInputStream::readPrefixImpl()
{
    TCurrentTraceContextGuard guard(TraceContext_);
    YT_LOG_DEBUG("readPrefixImpl() is called");

    IdleTimer_.Start();
}

void TBlockInputStream::readSuffixImpl()
{
    TCurrentTraceContextGuard guard(TraceContext_);
    YT_LOG_DEBUG("readSuffixImpl() is called");

    IdleTimer_.Stop();

    YT_LOG_DEBUG(
        "Block input stream timing statistics (ColumnarConversionCpuTime: %v, NonColumnarConvertionCpuTime: %v, "
        "ConversionSyncWaitTime: %v, IdleTime: %v, ReadCount: %v)",
        ColumnarConversionCpuTime_,
        NonColumnarConversionCpuTime_,
        ConversionSyncWaitTime_,
        IdleTimer_.GetElapsedTime(),
        ReadCount_);

    if (TraceContext_) {
        TraceContext_->AddTag("chyt.reader.data_statistics", Reader_->GetDataStatistics());
        TraceContext_->AddTag("chyt.reader.codec_statistics", Reader_->GetDecompressionStatistics());
        TraceContext_->AddTag("chyt.reader.timing_statistics", Reader_->GetTimingStatistics());
        TraceContext_->AddTag("chyt.reader.idle_time", IdleTimer_.GetElapsedTime());
        if (ColumnarConversionCpuTime_ != TDuration::Zero()) {
            TraceContext_->AddTag("chyt.reader.columnar_conversion_cpu_time", ColumnarConversionCpuTime_);
        }
        if (NonColumnarConversionCpuTime_ != TDuration::Zero()) {
            TraceContext_->AddTag("chyt.reader.non_columnar_conversion_cpu_time", NonColumnarConversionCpuTime_);
        }
        if (ConversionSyncWaitTime_ != TDuration::Zero()) {
            TraceContext_->AddTag("chyt.reader.conversion_sync_wait_time", ConversionSyncWaitTime_);
        }
        // TODO(dakovalkov): https://st.yandex-team.ru/YT-14032
        // Delete this statistics when GetTimingStatistics() works properly for TSchemalessMergingMultiChunkReader.
        if (WaitReadyEventTime_ != TDuration::Zero()) {
            TraceContext_->AddTag("chyt.reader.wait_ready_event_time", WaitReadyEventTime_);
        }
        TraceContext_->Finish();
    }
}

DB::Block TBlockInputStream::readImpl()
{
    std::optional<TCurrentTraceContextGuard> guard;
    if (Settings_->EnableReaderTracing) {
        guard.emplace(TraceContext_);
    }

    IdleTimer_.Stop();
    ++ReadCount_;

    NProfiling::TWallTimer totalWallTimer;
    YT_LOG_TRACE("Started reading ClickHouse block");

    DB::Block block;
    while (block.rows() == 0) {
        TRowBatchReadOptions options{
            // .MaxRowsPerRead = 100 * 1000,
            // .MaxDataWeightPerRead = 160_MB,
            .Columnar = Settings_->EnableColumnarRead,
        };
        auto batch = Reader_->Read(options);
        if (!batch) {
            return {};
        }
        if (batch->IsEmpty()) {
            NProfiling::TWallTimer wallTimer;
            WaitFor(Reader_->GetReadyEvent())
                .ThrowOnError();

            auto elapsed = wallTimer.GetElapsedTime();
            WaitReadyEventTime_ += elapsed;

            if (elapsed > TDuration::Seconds(1)) {
                YT_LOG_DEBUG("Reading took significant time (WallTime: %v)", elapsed);
            }
            continue;
        }

        {
            if (Settings_->ConvertRowBatchesInWorkerThreadPool) {
                auto start = TInstant::Now();
                block = WaitFor(BIND(&TBlockInputStream::ConvertRowBatchToBlock, this, batch)
                    .AsyncVia(Host_->GetClickHouseWorkerInvoker())
                    .Run())
                    .ValueOrThrow();
                auto finish = TInstant::Now();
                ConversionSyncWaitTime_ += finish - start;
            } else {
                block = ConvertRowBatchToBlock(batch);
            }
        }

        if (PrewhereInfo_) {
            block = FilterRowsByPrewhereInfo(std::move(block), PrewhereInfo_);
        }

        // NB: ConvertToField copies all strings, so clearing row buffer is safe here.
        RowBuffer_->Clear();
    }

    auto totalElapsed = totalWallTimer.GetElapsedTime();
    YT_LOG_TRACE("Finished reading ClickHouse block (WallTime: %v)", totalElapsed);

    IdleTimer_.Start();

    return block;
}

void TBlockInputStream::Prepare()
{
    InputHeaderBlock_ = ToHeaderBlock(*ReadSchemaWithVirtualColumns_, Settings_->Composite);
    OutputHeaderBlock_ = ToHeaderBlock(*ReadSchemaWithVirtualColumns_, Settings_->Composite);

    if (PrewhereInfo_) {
        // Create header with executed prewhere actions.
        ExecutePrewhereActions(OutputHeaderBlock_, PrewhereInfo_);
    }

    for (int index = 0; index < static_cast<int>(ReadSchemaWithVirtualColumns_->Columns().size()); ++index) {
        const auto& columnSchema = ReadSchemaWithVirtualColumns_->Columns()[index];
        auto id = Reader_->GetNameTable()->GetIdOrRegisterName(columnSchema.Name());
        if (static_cast<int>(IdToColumnIndex_.size()) <= id) {
            IdToColumnIndex_.resize(id + 1, -1);
        }
        IdToColumnIndex_[id] = index;
    }
}

DB::Block TBlockInputStream::ConvertRowBatchToBlock(const IUnversionedRowBatchPtr& batch)
{
    bool isColumnarBatch = static_cast<bool>(batch->TryAsColumnar());

    NProfiling::TWallTimer timer;
    auto result = ToBlock(
        batch,
        *ReadSchemaWithVirtualColumns_,
        IdToColumnIndex_,
        RowBuffer_,
        InputHeaderBlock_,
        Settings_->Composite);

    if (isColumnarBatch) {
        ColumnarConversionCpuTime_ += timer.GetElapsedTime();
    } else {
        NonColumnarConversionCpuTime_ += timer.GetElapsedTime();
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<TBlockInputStream> CreateBlockInputStream(
    ISchemalessMultiChunkReaderPtr reader,
    TTableSchemaPtr readSchema,
    TTraceContextPtr traceContext,
    THost* host,
    TQuerySettingsPtr querySettings,
    TLogger logger,
    DB::PrewhereInfoPtr prewhereInfo)
{
    return std::make_shared<TBlockInputStream>(
        std::move(reader),
        std::move(readSchema),
        std::move(traceContext),
        host,
        querySettings,
        logger,
        std::move(prewhereInfo));
}

////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<TBlockInputStream> CreateBlockInputStream(
    TStorageContext* storageContext,
    const TSubquerySpec& subquerySpec,
    const std::vector<TString>& realColumns,
    const std::vector<TString>& virtualColumns,
    const NTracing::TTraceContextPtr& traceContext,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    DB::PrewhereInfoPtr prewhereInfo)
{
    auto* queryContext = storageContext->QueryContext;
    auto blockReadOptions = CreateBlockReadOptions(queryContext->User);

    auto readSchema = subquerySpec.ReadSchema->Filter(realColumns);
    auto readSchemaWithVirtualColumns = InsertVirtualColumns(readSchema, subquerySpec.DataSourceDirectory, virtualColumns);

    auto blockInputStreamTraceContext = NTracing::CreateChildTraceContext(
        traceContext,
        "ClickHouseYt.BlockInputStream");

    std::optional<NTracing::TCurrentTraceContextGuard> guard;
    if (storageContext->Settings->EnableReaderTracing) {
        guard.emplace(blockInputStreamTraceContext);
    }

    ISchemalessMultiChunkReaderPtr reader;

    auto readerMemoryManager = queryContext->Host->GetMultiReaderMemoryManager()->CreateMultiReaderMemoryManager(
        queryContext->Host->GetConfig()->ReaderMemoryRequirement,
        {queryContext->UserTagId});

    auto defaultTableReaderConfig = queryContext->Host->GetConfig()->TableReader;
    auto tableReaderConfig = storageContext->Settings->TableReader;
    tableReaderConfig = UpdateYsonSerializable(defaultTableReaderConfig, ConvertToNode(tableReaderConfig));

    tableReaderConfig->SamplingMode = subquerySpec.TableReaderConfig->SamplingMode;
    tableReaderConfig->SamplingRate = subquerySpec.TableReaderConfig->SamplingRate;
    tableReaderConfig->SamplingSeed = subquerySpec.TableReaderConfig->SamplingSeed;
    TLogger Logger(queryContext->Logger);

    if (!subquerySpec.DataSourceDirectory->DataSources().empty() &&
        subquerySpec.DataSourceDirectory->DataSources()[0].GetType() == EDataSourceType::VersionedTable)
    {
        std::vector<NChunkClient::NProto::TChunkSpec> chunkSpecs;
        for (const auto& dataSliceDescriptor : dataSliceDescriptors) {
            for (auto& chunkSpec : dataSliceDescriptor.ChunkSpecs) {
                chunkSpecs.emplace_back(std::move(chunkSpec));
            }
        }
        // TODO(dakovalkov): I think we lost VirtualRowIndex here.
        TDataSliceDescriptor dataSliceDescriptor(std::move(chunkSpecs));

        reader = CreateSchemalessMergingMultiChunkReader(
            std::move(tableReaderConfig),
            New<NTableClient::TTableReaderOptions>(),
            queryContext->Client(),
            /* localDescriptor */ {},
            /* partitionTag */ std::nullopt,
            queryContext->Client()->GetNativeConnection()->GetBlockCache(),
            queryContext->Client()->GetNativeConnection()->GetNodeDirectory(),
            subquerySpec.DataSourceDirectory,
            dataSliceDescriptor,
            TNameTable::FromSchema(*readSchemaWithVirtualColumns),
            blockReadOptions,
            TColumnFilter(readSchemaWithVirtualColumns->GetColumnCount()),
            /* trafficMeter */ nullptr,
            GetUnlimitedThrottler(),
            GetUnlimitedThrottler(),
            /* multiReaderMemoryManager = */readerMemoryManager);
    } else {
        reader = CreateSchemalessParallelMultiReader(
            std::move(tableReaderConfig),
            New<NTableClient::TTableReaderOptions>(),
            queryContext->Client(),
            /* localDescriptor =*/{},
            std::nullopt,
            queryContext->Client()->GetNativeConnection()->GetBlockCache(),
            queryContext->Client()->GetNativeConnection()->GetNodeDirectory(),
            subquerySpec.DataSourceDirectory,
            dataSliceDescriptors,
            TNameTable::FromSchema(*readSchemaWithVirtualColumns),
            blockReadOptions,
            TColumnFilter(readSchemaWithVirtualColumns->GetColumnCount()),
            /* keyColumns =*/{},
            /* partitionTag =*/std::nullopt,
            /* trafficMeter =*/nullptr,
            /* bandwidthThrottler =*/GetUnlimitedThrottler(),
            /* rpsThrottler =*/GetUnlimitedThrottler(),
            /* multiReaderMemoryManager =*/readerMemoryManager);
    }

    return CreateBlockInputStream(
        std::move(reader),
        std::move(readSchemaWithVirtualColumns),
        blockInputStreamTraceContext,
        queryContext->Host,
        storageContext->Settings,
        queryContext->Logger.WithTag("ReadSessionId: %v", blockReadOptions.ReadSessionId),
        prewhereInfo);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer