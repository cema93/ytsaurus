#include "query_analyzer.h"

#include "helpers.h"
#include "query_context.h"
#include "subquery.h"
#include "table.h"
#include "computed_columns.h"
#include "format.h"
#include "query_context.h"
#include "host.h"
#include "config.h"
#include "std_helpers.h"

#include <yt/yt/ytlib/chunk_client/data_source.h>
#include <yt/yt/ytlib/chunk_client/legacy_data_slice.h>
#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/input_chunk.h>

#include <yt/yt/client/chunk_client/proto/chunk_meta.pb.h>

#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTFunction.h>
#include <Interpreters/DatabaseAndTableWithAlias.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/TableJoin.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/TranslateQualifiedNamesVisitor.h>

#include <library/cpp/string_utils/base64/base64.h>

namespace NYT::NClickHouseServer {

using namespace NChunkPools;
using namespace NChunkClient;
using namespace NTableClient;
using namespace NYPath;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

void FillDataSliceDescriptors(
    TSubquerySpec& subquerySpec,
    THashMap<TChunkId, TRefCountedMiscExtPtr> miscExtMap,
    const TRange<NChunkPools::TChunkStripePtr>& chunkStripes)
{
    for (const auto& chunkStripe : chunkStripes) {
        auto& inputDataSliceDescriptors = subquerySpec.DataSliceDescriptors.emplace_back();
        for (const auto& dataSlice : chunkStripe->DataSlices) {
            auto& inputDataSliceDescriptor = inputDataSliceDescriptors.emplace_back();
            for (const auto& chunkSlice : dataSlice->ChunkSlices) {
                auto& chunkSpec = inputDataSliceDescriptor.ChunkSpecs.emplace_back();
                ToProto(&chunkSpec, chunkSlice, /* comparator */ TComparator(), EDataSourceType::UnversionedTable);
                auto it = miscExtMap.find(chunkSlice->GetInputChunk()->GetChunkId());
                YT_VERIFY(it != miscExtMap.end());
                if (it->second) {
                    SetProtoExtension(
                        chunkSpec.mutable_chunk_meta()->mutable_extensions(),
                        static_cast<const NChunkClient::NProto::TMiscExt&>(*it->second));
                }
            }
            inputDataSliceDescriptor.VirtualRowIndex = dataSlice->VirtualRowIndex;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

TQueryAnalyzer::TQueryAnalyzer(const DB::Context& context, const TStorageContext* storageContext, const DB::SelectQueryInfo& queryInfo, const TLogger& logger)
    : Context_(context)
    , StorageContext_(storageContext)
    , QueryInfo_(queryInfo)
    , Logger(logger)
{ }

void TQueryAnalyzer::ValidateKeyColumns()
{
    const auto& analyzedJoin = QueryInfo_.syntax_analyzer_result->analyzed_join;
    YT_VERIFY(Storages_[0]);

    struct TJoinArgument
    {
        int Index;
        TTableSchemaPtr TableSchema;
        std::vector<TRichYPath> Paths;
        THashMap<TString, int> KeyColumnToIndex;
        std::vector<TString> JoinColumns;
    };

    std::vector<TJoinArgument> joinArguments;

    for (int index = 0; index < static_cast<int>(Storages_.size()); ++index) {
        if (const auto& storage = Storages_[index]) {
            auto& joinArgument = joinArguments.emplace_back();
            joinArgument.Index = index;
            joinArgument.TableSchema = storage->GetSchema();
            for (
                int columnIndex = 0;
                columnIndex < static_cast<int>(joinArgument.TableSchema->GetKeyColumns().size());
                ++columnIndex)
            {
                auto column = joinArgument.TableSchema->GetKeyColumns()[columnIndex];
                joinArgument.KeyColumnToIndex[column] = columnIndex;
            }

            auto tables = storage->GetTables();
            if (tables.size() == 1) {
                joinArgument.Paths = {tables.front()->Path};
            } else {
                THROW_ERROR_EXCEPTION("Invalid sorted JOIN: only single table may currently be joined")
                    << TErrorAttribute("table_index", index)
                    << TErrorAttribute("table_paths", joinArgument.Paths);
            }
        }
    }

    YT_VERIFY(joinArguments.size() >= 1);
    YT_VERIFY(joinArguments.size() <= 2);

    auto extractColumnNames = [] (const DB::ASTPtr& listAst) {
        std::vector<TString> result;
        for (const auto& keyAst : listAst->children) {
            if (auto* identifier = keyAst->as<DB::ASTIdentifier>()) {
                result.emplace_back(identifier->shortName());
            } else {
                THROW_ERROR_EXCEPTION("Invalid sorted JOIN: CHYT does not support compound expressions in ON/USING clause")
                    << TErrorAttribute("expression", keyAst);
            }
        }
        return result;
    };

    joinArguments[0].JoinColumns = extractColumnNames(analyzedJoin->leftKeysList());
    if (static_cast<int>(joinArguments.size()) == 2) {
        if (analyzedJoin->hasOn()) {
            joinArguments[1].JoinColumns = extractColumnNames(analyzedJoin->rightKeysList());
        } else {
            joinArguments[1].JoinColumns = joinArguments[0].JoinColumns;
        }
    }

    // Check that join columns occupy prefixes of the key columns.
    for (const auto& joinArgument : joinArguments) {
        int maxKeyColumnIndex = -1;
        for (const auto& joinColumn : joinArgument.JoinColumns) {
            auto it = joinArgument.KeyColumnToIndex.find(joinColumn);
            if (it == joinArgument.KeyColumnToIndex.end()) {
                THROW_ERROR_EXCEPTION("Invalid sorted JOIN: joined column %Qv is not a key column of table", joinColumn)
                    << TErrorAttribute("table_index", joinArgument.Index)
                    << TErrorAttribute("column", joinColumn)
                    << TErrorAttribute("key_columns", joinArgument.TableSchema->GetKeyColumns());
            }
            maxKeyColumnIndex = std::max(maxKeyColumnIndex, it->second);
        }
        if (maxKeyColumnIndex + 1 != static_cast<int>(joinArgument.JoinColumns.size())) {
            THROW_ERROR_EXCEPTION("Invalid sorted JOIN: joined columns should form prefix of joined table key columns")
                << TErrorAttribute("table_index", joinArgument.Index)
                << TErrorAttribute("join_columns", joinArgument.JoinColumns)
                << TErrorAttribute("key_columns", joinArgument.TableSchema->GetKeyColumns());
        }
    }

    if (joinArguments.size() == 2) {
        const auto& lhsSchema = joinArguments[0].TableSchema;
        const auto& rhsSchema = joinArguments[1].TableSchema;
        // Check that joined columns occupy same positions in both tables.
        for (int index = 0; index < static_cast<int>(joinArguments[0].JoinColumns.size()); ++index) {
            const auto& lhsColumn = joinArguments[0].JoinColumns[index];
            const auto& rhsColumn = joinArguments[1].JoinColumns[index];
            auto lhsIt = joinArguments[0].KeyColumnToIndex.find(lhsColumn);
            auto rhsIt = joinArguments[1].KeyColumnToIndex.find(rhsColumn);
            YT_VERIFY(lhsIt != joinArguments[0].KeyColumnToIndex.end());
            YT_VERIFY(rhsIt != joinArguments[1].KeyColumnToIndex.end());
            if (lhsIt->second != rhsIt->second) {
                THROW_ERROR_EXCEPTION(
                    "Invalid sorted JOIN: joined columns %Qv and %Qv do not occupy same positions in key columns of joined tables",
                    lhsColumn,
                    rhsColumn)
                    << TErrorAttribute("lhs_column", lhsColumn)
                    << TErrorAttribute("rhs_column", rhsColumn)
                    << TErrorAttribute("lhs_key_columns", lhsSchema->GetKeyColumns())
                    << TErrorAttribute("rhs_key_columns", rhsSchema->GetKeyColumns());
            }
        }
    }
}

void TQueryAnalyzer::ParseQuery()
{
    auto* selectQuery = QueryInfo_.query->as<DB::ASTSelectQuery>();

    YT_LOG_DEBUG("Analyzing query (Query: %v)", static_cast<const DB::IAST&>(*selectQuery));

    YT_VERIFY(selectQuery);
    YT_VERIFY(selectQuery->tables());

    auto* tablesInSelectQuery = selectQuery->tables()->as<DB::ASTTablesInSelectQuery>();
    YT_VERIFY(tablesInSelectQuery);
    YT_VERIFY(tablesInSelectQuery->children.size() >= 1);
    YT_VERIFY(tablesInSelectQuery->children.size() <= 2);

    for (int index = 0; index < static_cast<int>(tablesInSelectQuery->children.size()); ++index) {
        auto& tableInSelectQuery = tablesInSelectQuery->children[index];
        auto* tablesElement = tableInSelectQuery->as<DB::ASTTablesInSelectQueryElement>();
        YT_VERIFY(tablesElement);
        if (!tablesElement->table_expression) {
            // First element should always be table expression as it is the
            YT_VERIFY(index != 0);
            continue;
        }

        YT_LOG_DEBUG("Found table expression (Index: %v, TableExpression: %v)", index, *tablesElement->table_expression);

        if (index == 1) {
            Join_ = true;
        }

        auto& tableExpression = tablesElement->table_expression;

        if (tablesElement->table_join) {
            const auto* tableJoin = tablesElement->table_join->as<DB::ASTTableJoin>();
            YT_VERIFY(tableJoin);
            if (static_cast<int>(tableJoin->locality) == static_cast<int>(DB::ASTTableJoin::Locality::Global)) {
                YT_LOG_DEBUG("Table expression is a global join (Index: %v)", index);
                GlobalJoin_ = true;
            }
            if (static_cast<int>(tableJoin->kind) == static_cast<int>(DB::ASTTableJoin::Kind::Right) ||
                static_cast<int>(tableJoin->kind) == static_cast<int>(DB::ASTTableJoin::Kind::Full))
            {
                YT_LOG_DEBUG("Query is a right or full join");
                RightOrFullJoin_ = true;
            }
            if (static_cast<int>(tableJoin->kind) == static_cast<int>(DB::ASTTableJoin::Kind::Cross)) {
                YT_LOG_DEBUG("Query is a cross join");
                CrossJoin_ = true;
            }

        }

        TableExpressions_.emplace_back(tableExpression->as<DB::ASTTableExpression>());
        YT_VERIFY(TableExpressions_.back());
        TableExpressionPtrs_.emplace_back(&tableExpression);
    }

    // At least first table expression should be the one that instantiated this query analyzer (aka owner).
    YT_VERIFY(TableExpressions_.size() >= 1);
    // More than 2 tables are not supported in CH yet.
    YT_VERIFY(TableExpressions_.size() <= 2);

    for (size_t tableExpressionIndex = 0; tableExpressionIndex < TableExpressions_.size(); ++tableExpressionIndex) {
        const auto& tableExpression = TableExpressions_[tableExpressionIndex];

        if (tableExpressionIndex == 1 && GlobalJoin_) {
            // Table expression is the one replaced in GlobalSubqueriesVisitor
            // (like _data1 or sometimes just original table alias which now stands for external table).
            YT_LOG_DEBUG("Skipping table expression 1 due to global join (TableExpression: %v)",
                static_cast<DB::IAST&>(*tableExpression));
        } else {
            auto& storage = Storages_.emplace_back(GetStorage(tableExpression));
            if (storage) {
                YT_LOG_DEBUG("Table expression corresponds to TStorageDistributor (TableExpression: %v)",
                    static_cast<DB::IAST&>(*tableExpression));
                ++YtTableCount_;
            } else {
                YT_LOG_DEBUG("Table expression does not correspond to TStorageDistributor (TableExpression: %v)",
                    static_cast<DB::IAST&>(*tableExpression));
            }
        }

        if (Join_) {
            // Validate that if join is present, all table expressions have aliases.
            DB::ASTWithAlias* astWithAlias = nullptr;
            if (tableExpression->database_and_table_name) {
                astWithAlias = dynamic_cast<DB::ASTWithAlias*>(tableExpression->database_and_table_name.get());
            } else if (tableExpression->table_function) {
                astWithAlias = dynamic_cast<DB::ASTWithAlias*>(tableExpression->table_function.get());
            } else if (tableExpression->subquery) {
                astWithAlias = dynamic_cast<DB::ASTWithAlias*>(tableExpression->subquery.get());
            } else {
                YT_VERIFY(false);
            }
            YT_VERIFY(astWithAlias);
            if (astWithAlias->alias.empty()) {
                THROW_ERROR_EXCEPTION("In queries with JOIN all joined expressions should be provided with aliases")
                    << TErrorAttribute("table_expression", tableExpression);
            }
        }
    }

    YT_VERIFY(YtTableCount_ > 0);

    if (YtTableCount_ == 2) {
        if (!CrossJoin_) {
            YT_LOG_DEBUG("Query is a two-YT-table join");
            TwoYTTableJoin_ = true;
        } else {
            YT_LOG_DEBUG("Query is a two-YT-table cross join; considering this as a single YT table join");
            YtTableCount_ = 1;
            TwoYTTableJoin_ = false;
            TableExpressions_.pop_back();
            TableExpressionPtrs_.pop_back();
            Storages_.pop_back();
        }
    }

    YT_LOG_DEBUG(
        "Extracted table expressions from query (Query: %v, TableExpressionCount: %v, YtTableCount: %v, "
        "IsJoin: %v, IsGlobalJoin: %v, IsRightOrFullJoin: %v, IsCrossJoin: %v)",
        *QueryInfo_.query,
        TableExpressions_.size(),
        YtTableCount_,
        Join_,
        GlobalJoin_,
        RightOrFullJoin_,
        CrossJoin_);
}

TQueryAnalysisResult TQueryAnalyzer::Analyze()
{
    ParseQuery();

    if ((TwoYTTableJoin_ && !CrossJoin_) || RightOrFullJoin_) {
        ValidateKeyColumns();
    }

    TQueryAnalysisResult result;

    const auto& settings = StorageContext_->Settings;

    for (const auto& storage : Storages_) {
        if (!storage) {
            continue;
        }
        result.Tables.emplace_back(storage->GetTables());
        auto schema = storage->GetSchema();
        std::optional<DB::KeyCondition> keyCondition;
        if (schema->IsSorted()) {
            auto primaryKeyExpression = std::make_shared<DB::ExpressionActions>(std::make_shared<DB::ActionsDAG>(
                ToNamesAndTypesList(*schema, settings->Composite)));

            auto queryInfoForKeyCondition = QueryInfo_;

            if (settings->EnableComputedColumnDeduction) {
                // Query may not contain deducible values for computed columns.
                // We populate query with deducible equations on computed columns,
                // so key condition is able to properly filter ranges with computed
                // key columns.
                queryInfoForKeyCondition.query = queryInfoForKeyCondition.query->clone();
                auto* selectQuery = queryInfoForKeyCondition.query->as<DB::ASTSelectQuery>();
                YT_VERIFY(selectQuery);

                if (selectQuery->where()) {
                    selectQuery->refWhere() = PopulatePredicateWithComputedColumns(
                        selectQuery->where(),
                        schema,
                        Context_,
                        queryInfoForKeyCondition.sets,
                        settings,
                        Logger);
                }
            }

            keyCondition = DB::KeyCondition(queryInfoForKeyCondition, Context_, ToNames(schema->GetKeyColumns()), primaryKeyExpression);
        }
        result.KeyConditions.emplace_back(std::move(keyCondition));
        result.TableSchemas.emplace_back(storage->GetSchema());
    }

    if ((TwoYTTableJoin_ && !CrossJoin_) || RightOrFullJoin_) {
        result.PoolKind = EPoolKind::Sorted;
        result.KeyColumnCount = KeyColumnCount_ = QueryInfo_.syntax_analyzer_result->analyzed_join->leftKeysList()->children.size();
    } else {
        result.PoolKind = EPoolKind::Unordered;
    }

    return result;
}

DB::ASTPtr TQueryAnalyzer::RewriteQuery(
    const TRange<TSubquery> threadSubqueries,
    TSubquerySpec specTemplate,
    const THashMap<TChunkId, TRefCountedMiscExtPtr>& miscExtMap,
    int subqueryIndex,
    bool isLastSubquery)
{
    auto Logger = this->Logger.WithTag("SubqueryIndex: %v", subqueryIndex);

    i64 totalRowCount = 0;
    i64 totalDataWeight = 0;
    i64 totalChunkCount = 0;
    for (const auto& subquery : threadSubqueries) {
        const auto& stripeList = subquery.StripeList;
        totalRowCount += stripeList->TotalRowCount;
        totalDataWeight += stripeList->TotalDataWeight;
        totalChunkCount += stripeList->TotalChunkCount;
    }

    YT_LOG_DEBUG(
        "Rewriting query (YtTableCount: %v, ThreadSubqueryCount: %v, TotalDataWeight: %v, TotalRowCount: %v, TotalChunkCount: %v)",
        YtTableCount_,
        threadSubqueries.size(),
        totalDataWeight,
        totalRowCount,
        totalChunkCount);

    specTemplate.SubqueryIndex = subqueryIndex;

    std::vector<DB::ASTPtr> newTableExpressions;

    for (int index = 0; index < YtTableCount_; ++index) {
        auto tableExpression = TableExpressions_[index];

        std::vector<TChunkStripePtr> stripes;
        for (const auto& subquery : threadSubqueries) {
            stripes.emplace_back(subquery.StripeList->Stripes[index]);
        }

        auto spec = specTemplate;
        spec.TableIndex = index;
        spec.ReadSchema = Storages_[index]->GetSchema();

        FillDataSliceDescriptors(spec, miscExtMap, MakeRange(stripes));

        auto protoSpec = NYT::ToProto<NProto::TSubquerySpec>(spec);
        auto encodedSpec = Base64Encode(protoSpec.SerializeAsString());

        YT_LOG_DEBUG("Serializing subquery spec (TableIndex: %v, SpecLength: %v)", index, encodedSpec.size());

        auto tableFunction = makeASTFunction("ytSubquery", std::make_shared<DB::ASTLiteral>(std::string(encodedSpec.data())));

        if (tableExpression->database_and_table_name) {
            DB::DatabaseAndTableWithAlias databaseAndTable(tableExpression->database_and_table_name);
            if (!databaseAndTable.alias.empty()) {
                tableFunction->alias = databaseAndTable.alias;
            } else {
                tableFunction->alias = databaseAndTable.table;
            }
        } else {
            tableFunction->alias = static_cast<DB::ASTWithAlias&>(*tableExpression->table_function).alias;
        }

        auto clonedTableExpression = tableExpression->clone();
        auto* typedTableExpression = clonedTableExpression->as<DB::ASTTableExpression>();
        YT_VERIFY(typedTableExpression);
        typedTableExpression->table_function = std::move(tableFunction);
        typedTableExpression->database_and_table_name = nullptr;
        typedTableExpression->subquery = nullptr;
        typedTableExpression->sample_offset = nullptr;
        typedTableExpression->sample_size = nullptr;

        newTableExpressions.emplace_back(std::move(clonedTableExpression));
    }

    ReplaceTableExpressions(newTableExpressions);

    // TODO(max42): this comparator should be created beforehand.
    TComparator comparator(std::vector<ESortOrder>(KeyColumnCount_, ESortOrder::Ascending));

    if (RightOrFullJoin_) {
        TOwningKeyBound lowerBound;
        if (PreviousUpperBound_) {
            lowerBound = PreviousUpperBound_.Invert();
        }
        TOwningKeyBound upperBound;
        if (!isLastSubquery) {
            upperBound = threadSubqueries.Back().Bounds.second;
        }
        PreviousUpperBound_ = upperBound;
        if (lowerBound) {
            YT_VERIFY(!upperBound || !comparator.IsRangeEmpty(lowerBound, upperBound));
        }
        AppendWhereCondition(lowerBound, upperBound);
    }

    auto result = QueryInfo_.query->clone();

    RollbackModifications();

    YT_LOG_TRACE("Restoring qualified names (QueryBefore: %v)", *result);

    DB::RestoreQualifiedNamesVisitor::Data data;
    DB::RestoreQualifiedNamesVisitor(data).visit(result);

    YT_LOG_DEBUG("Query rewritten (NewQuery: %v)", *result);

    return result;
}

std::shared_ptr<IStorageDistributor> TQueryAnalyzer::GetStorage(const DB::ASTTableExpression* tableExpression) const
{
    if (!tableExpression) {
        return nullptr;
    }
    DB::StoragePtr storage;
    if (tableExpression->table_function) {
        storage = const_cast<DB::Context&>(Context_.getQueryContext()).executeTableFunction(tableExpression->table_function);
    } else if (tableExpression->database_and_table_name) {
        auto databaseAndTable = DB::DatabaseAndTableWithAlias(tableExpression->database_and_table_name);
        if (databaseAndTable.database.empty()) {
            databaseAndTable.database = "YT";
        }
        storage = DB::DatabaseCatalog::instance().getTable({databaseAndTable.database, databaseAndTable.table}, Context_);
    }

    return std::dynamic_pointer_cast<IStorageDistributor>(storage);
}

void TQueryAnalyzer::ApplyModification(DB::ASTPtr* queryPart, DB::ASTPtr newValue, DB::ASTPtr previousValue)
{
    YT_LOG_DEBUG("Replacing query part (QueryPart: %v, NewValue: %v)", previousValue, newValue);
    Modifications_.emplace_back(queryPart, std::move(previousValue));
    *queryPart = std::move(newValue);
}

void TQueryAnalyzer::ApplyModification(DB::ASTPtr* queryPart, DB::ASTPtr newValue)
{
    ApplyModification(queryPart, newValue, *queryPart);
}

void TQueryAnalyzer::RollbackModifications()
{
    YT_LOG_DEBUG("Rolling back modifications (ModificationCount: %v)", Modifications_.size());
    while (!Modifications_.empty()) {
        auto& [queryPart, oldValue] = Modifications_.back();
        *queryPart = std::move(oldValue);
        Modifications_.pop_back();
    }
}

void TQueryAnalyzer::AppendWhereCondition(
    TOwningKeyBound lowerBound,
    TOwningKeyBound upperBound)
{
    auto keyAsts = QueryInfo_.syntax_analyzer_result->analyzed_join->leftKeysList()->children;
    YT_LOG_DEBUG("Appending where-condition (LowerLimit: %v, UpperLimit: %v)", lowerBound, upperBound);

    auto createFunction = [] (std::string name, std::vector<DB::ASTPtr> params) {
        auto function = std::make_shared<DB::ASTFunction>();
        function->name = std::move(name);
        function->arguments = std::make_shared<DB::ASTExpressionList>();
        function->children.push_back(function->arguments);
        function->arguments->children = std::move(params);
        return function;
    };

    auto unversionedRowToTuple = [&] (TUnversionedRow row) {
        std::vector<DB::ASTPtr> literals;
        literals.reserve(row.GetCount());
        for (const auto& value : row) {
            auto field = ToField(value);
            literals.emplace_back(std::make_shared<DB::ASTLiteral>(field));
        }
        return createFunction("tuple", std::move(literals));
    };

    std::vector<DB::ASTPtr> conjunctionArgs = {};

    auto keyTuple = createFunction("tuple", keyAsts);

    if (lowerBound) {
        YT_VERIFY(!lowerBound.IsUpper);
        auto lowerTuple = unversionedRowToTuple(lowerBound.Prefix);
        auto lowerFunctionName = lowerBound.IsInclusive ? "greaterOrEquals" : "greater";
        auto lowerFunction = createFunction(lowerFunctionName, {keyTuple, lowerTuple});
        conjunctionArgs.emplace_back(std::move(lowerFunction));
    }

    if (upperBound) {
        YT_VERIFY(upperBound.IsUpper);
        auto upperTuple = unversionedRowToTuple(upperBound.Prefix);
        auto upperFunctionName = upperBound.IsInclusive ? "lessOrEquals" : "less";
        auto upperFunction = createFunction(upperFunctionName, {keyTuple, upperTuple});
        conjunctionArgs.emplace_back(std::move(upperFunction));
    }

    auto* selectQuery = QueryInfo_.query->as<DB::ASTSelectQuery>();
    YT_VERIFY(selectQuery);
    if (selectQuery->where()) {
        conjunctionArgs.emplace_back(selectQuery->where());
    }

    if (conjunctionArgs.empty()) {
        return;
    }

    // TODO(max42): why do we need this?
    for (auto& arg : conjunctionArgs) {
        arg = createFunction("assumeNotNull", {std::move(arg)});
    }

    DB::ASTPtr newWhere;
    if (static_cast<int>(conjunctionArgs.size()) == 1) {
        newWhere = conjunctionArgs.front();
    } else {
        newWhere = createFunction("and", conjunctionArgs);
    }

    auto previousWhere = selectQuery->where();
    if (!previousWhere) {
        // NB: refWhere() has a weird behavior which does not allow you to call it
        // unless something has been previously set.
        selectQuery->setExpression(DB::ASTSelectQuery::Expression::WHERE, std::make_shared<DB::ASTFunction>());
    }
    ApplyModification(&selectQuery->refWhere(), std::move(newWhere), std::move(previousWhere));
}

void TQueryAnalyzer::ReplaceTableExpressions(std::vector<DB::ASTPtr> newTableExpressions)
{
    YT_VERIFY(static_cast<int>(newTableExpressions.size()) == YtTableCount_);
    for (int index = 0; index < static_cast<int>(newTableExpressions.size()); ++index) {
        YT_VERIFY(newTableExpressions[index]);
        ApplyModification(TableExpressionPtrs_[index], newTableExpressions[index]);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer