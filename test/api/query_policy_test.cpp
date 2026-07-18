#include "api_test/api_test.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

#include "parser/query/graph_pattern/rel_pattern.h"

using namespace lbug::common;
using namespace lbug::testing;

namespace {

static_assert(LBUG_QUERY_METADATA_ABI_VERSION == 1);

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::ranges::find(values, value) != values.end();
}

void expectSortedAndUnique(const std::vector<std::string>& values) {
    EXPECT_TRUE(std::ranges::is_sorted(values));
    EXPECT_EQ(std::ranges::adjacent_find(values), values.end());
}

void createPolicyTestSchema(lbug::main::Connection& connection) {
    auto result = connection.query(
        "CREATE NODE TABLE person(ID INT64, fName STRING, PRIMARY KEY(ID));"
        "CREATE REL TABLE knows(FROM person TO person);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
}

} // namespace

TEST_F(ApiTest, ParserPatternLegacyEmptyBraceConstructionIsUnambiguous) {
    lbug::parser::NodePattern node{"n", {}, {}};
    lbug::parser::RelPattern rel{"r", {}, QueryRelType::NON_RECURSIVE,
        lbug::parser::ArrowDirection::RIGHT, {}, {}};

    EXPECT_TRUE(node.getTableNames().empty());
    EXPECT_TRUE(rel.getTableNames().empty());
}

TEST_F(ApiTest, PreparedStatementMetadataUsesDecodedParsedNames) {
    createPolicyTestSchema(*conn);
    auto prepared = conn->prepare(R"(
        MATCH /* (:IgnoredLabel) $ignored */
              (`_node``name`:`person`)-[`_rel``name`:`knows`]->(`friend`)
        WHERE `_node``name`.`fName` = $`pa``ram`
        RETURN `_node``name`.`fName` AS `out``put`
    )");

    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    const auto& metadata = prepared->getMetadata();
    EXPECT_EQ(metadata.getParameterNames(), std::vector<std::string>{"pa`ram"});
    EXPECT_EQ(metadata.getNodeLabels(), std::vector<std::string>{"person"});
    EXPECT_EQ(metadata.getRelLabels(), std::vector<std::string>{"knows"});
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "_node`name"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "_rel`name"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "fName"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "out`put"));
    EXPECT_FALSE(contains(metadata.getIdentifiers(), "IgnoredLabel"));
    expectSortedAndUnique(metadata.getParameterNames());
    expectSortedAndUnique(metadata.getIdentifiers());
    expectSortedAndUnique(metadata.getNodeLabels());
    expectSortedAndUnique(metadata.getRelLabels());

    auto result = conn->execute(prepared.get(),
        std::make_pair(std::string{"pa`ram"}, std::string{"Alice"}));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
}

TEST_F(ApiTest, PreparedStatementMetadataIncludesQualifiedLabelComponentsAsIdentifiers) {
    auto prepared = conn->prepare(
        "MATCH (n:analytics.`__Rhiza``Node`)-[:archive.`__Rhiza``Rel`]->(m) RETURN n");

    ASSERT_FALSE(prepared->isSuccess());
    const auto& metadata = prepared->getMetadata();
    EXPECT_TRUE(contains(metadata.getNodeLabels(), "analytics.__Rhiza`Node"));
    EXPECT_TRUE(contains(metadata.getRelLabels(), "archive.__Rhiza`Rel"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "analytics.__Rhiza`Node"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "analytics"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "__Rhiza`Node"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "archive.__Rhiza`Rel"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "archive"));
    EXPECT_TRUE(contains(metadata.getIdentifiers(), "__Rhiza`Rel"));

    auto escapedDot =
        conn->prepare("MATCH (n:`db.__Rhiza`)-[:`archive.__RhizaRel`]->(m) RETURN n");
    ASSERT_FALSE(escapedDot->isSuccess());
    const auto& escapedDotMetadata = escapedDot->getMetadata();
    EXPECT_TRUE(contains(escapedDotMetadata.getNodeLabels(), "db.__Rhiza"));
    EXPECT_TRUE(contains(escapedDotMetadata.getRelLabels(), "archive.__RhizaRel"));
    EXPECT_TRUE(contains(escapedDotMetadata.getIdentifiers(), "db.__Rhiza"));
    EXPECT_TRUE(contains(escapedDotMetadata.getIdentifiers(), "archive.__RhizaRel"));
    EXPECT_FALSE(contains(escapedDotMetadata.getIdentifiers(), "db"));
    EXPECT_FALSE(contains(escapedDotMetadata.getIdentifiers(), "__Rhiza"));
    EXPECT_FALSE(contains(escapedDotMetadata.getIdentifiers(), "archive"));
    EXPECT_FALSE(contains(escapedDotMetadata.getIdentifiers(), "__RhizaRel"));
}

TEST_F(ApiTest, PreparedStatementMetadataReportsPatternsAndQueryFeatures) {
    createPolicyTestSchema(*conn);
    auto patterns = conn->prepare(
        "MATCH (a:person)-[e:knows]->(b), (c)-[r]->(d:person) RETURN count(*)");
    ASSERT_TRUE(patterns->isSuccess()) << patterns->getErrorMessage();
    const auto& patternMetadata = patterns->getMetadata();
    EXPECT_TRUE(patternMetadata.hasUnlabeledNodePattern());
    EXPECT_TRUE(patternMetadata.hasUnlabeledRelPattern());
    EXPECT_EQ(patternMetadata.getNodeLabels(), std::vector<std::string>{"person"});
    EXPECT_EQ(patternMetadata.getRelLabels(), std::vector<std::string>{"knows"});

    auto clauses = conn->prepare(
        "MATCH (a:person) OPTIONAL MATCH (a)-[:knows*1..2]->(b:person) "
        "UNWIND [a] AS item RETURN item");
    ASSERT_TRUE(clauses->isSuccess()) << clauses->getErrorMessage();
    const auto& clauseMetadata = clauses->getMetadata();
    EXPECT_TRUE(clauseMetadata.hasOptionalMatch());
    EXPECT_TRUE(clauseMetadata.hasUnwind());
    EXPECT_TRUE(clauseMetadata.hasRecursiveRelPattern());

    auto subquery = conn->prepare(
        "MATCH (a:person) WHERE EXISTS { MATCH (a)-[:knows]->(`_nested``node`) "
        "WHERE `_nested``node`.`fName` = $`nested``param` } RETURN a");
    ASSERT_TRUE(subquery->isSuccess()) << subquery->getErrorMessage();
    EXPECT_EQ(subquery->getMetadata().getParameterNames(),
        std::vector<std::string>{"nested`param"});
    EXPECT_TRUE(contains(subquery->getMetadata().getIdentifiers(), "_nested`node"));
    EXPECT_TRUE(contains(subquery->getMetadata().getIdentifiers(), "fName"));
    EXPECT_EQ(subquery->getMetadata().getRelLabels(), std::vector<std::string>{"knows"});

    auto unionDistinct = conn->prepare("RETURN 1 AS n UNION RETURN 2 AS n");
    ASSERT_TRUE(unionDistinct->isSuccess()) << unionDistinct->getErrorMessage();
    EXPECT_TRUE(unionDistinct->getMetadata().hasUnion());
    EXPECT_FALSE(unionDistinct->getMetadata().hasUnionAll());

    auto unionAll = conn->prepare("RETURN 1 AS n UNION ALL RETURN 2 AS n");
    ASSERT_TRUE(unionAll->isSuccess()) << unionAll->getErrorMessage();
    EXPECT_TRUE(unionAll->getMetadata().hasUnion());
    EXPECT_TRUE(unionAll->getMetadata().hasUnionAll());

    auto call = conn->prepare("CALL show_tables() RETURN *");
    ASSERT_TRUE(call->isSuccess()) << call->getErrorMessage();
    EXPECT_TRUE(call->getMetadata().hasInQueryCall());

    const auto csvPath = TestHelper::getTempDir("query_policy_load") + "/rows.csv";
    std::ofstream{csvPath} << "ID\n1\n";
    auto load = conn->prepare("LOAD FROM '" + csvPath + "' RETURN ID LIMIT 1");
    ASSERT_TRUE(load->isSuccess()) << load->getErrorMessage();
    EXPECT_TRUE(load->getMetadata().hasLoadFrom());
}

TEST_F(ApiTest, PreparedStatementMetadataSnapshotsTypeAndReadOnlyIndependently) {
    createPolicyTestSchema(*conn);
    auto read = conn->prepare("RETURN $value");
    ASSERT_TRUE(read->isSuccess()) << read->getErrorMessage();
    EXPECT_EQ(read->getStatementType(), StatementType::QUERY);
    EXPECT_TRUE(read->isReadOnly());
    EXPECT_EQ(read->getMetadata().getStatementType(), read->getStatementType());
    EXPECT_EQ(read->getMetadata().isReadOnly(), read->isReadOnly());
    EXPECT_EQ(read->getMetadata().getParameterNames(), std::vector<std::string>{"value"});

    auto result = conn->execute(read.get(), std::make_pair(std::string{"value"}, int64_t{1}));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    EXPECT_EQ(read->getMetadata().getParameterNames(), std::vector<std::string>{"value"});

    auto write = conn->prepare("CREATE (:person {ID: 123456})");
    ASSERT_TRUE(write->isSuccess()) << write->getErrorMessage();
    EXPECT_EQ(write->getStatementType(), StatementType::QUERY);
    EXPECT_FALSE(write->isReadOnly());
    EXPECT_EQ(write->getMetadata().getStatementType(), write->getStatementType());
    EXPECT_EQ(write->getMetadata().isReadOnly(), write->isReadOnly());
}

TEST_F(ApiTest, QueryMaxOutputRowsIsAnExactPerExecutionLimit) {
    auto exact = conn->query("UNWIND range(1, 3) AS x RETURN x", 3);
    ASSERT_TRUE(exact->isSuccess()) << exact->getErrorMessage();
    EXPECT_EQ(exact->getNumTuples(), 3);

    auto exceeded = conn->query("UNWIND range(1, 3) AS x RETURN x", 2);
    ASSERT_FALSE(exceeded->isSuccess());
    EXPECT_NE(exceeded->getErrorMessage().find("maximum output row limit of 2"),
        std::string::npos);

    auto zero = conn->query("RETURN 1", 0);
    ASSERT_FALSE(zero->isSuccess());
    EXPECT_NE(zero->getErrorMessage().find("maximum output row limit of 0"), std::string::npos);

    auto unlimited = conn->query("UNWIND range(1, 3) AS x RETURN x");
    ASSERT_TRUE(unlimited->isSuccess()) << unlimited->getErrorMessage();
    EXPECT_EQ(unlimited->getNumTuples(), 3);

    auto factorizedExact =
        conn->query("UNWIND [1, 2] AS x UNWIND [3, 4, 5] AS y RETURN x, y", 6);
    ASSERT_TRUE(factorizedExact->isSuccess()) << factorizedExact->getErrorMessage();
    EXPECT_EQ(factorizedExact->getNumTuples(), 6);

    auto factorizedExceeded =
        conn->query("UNWIND [1, 2] AS x UNWIND [3, 4, 5] AS y RETURN x, y", 5);
    ASSERT_FALSE(factorizedExceeded->isSuccess());
    EXPECT_NE(factorizedExceeded->getErrorMessage().find("maximum output row limit of 5"),
        std::string::npos);
}

TEST_F(ApiTest, QueryMaxOutputRowsAppliesToActualRowsReturnedByRootSinks) {
    auto explain = conn->query("EXPLAIN RETURN 1", 1);
    ASSERT_TRUE(explain->isSuccess()) << explain->getErrorMessage();
    EXPECT_EQ(explain->getNumTuples(), 1);

    auto explainZero = conn->query("EXPLAIN RETURN 1", 0);
    ASSERT_FALSE(explainZero->isSuccess());
    EXPECT_NE(explainZero->getErrorMessage().find("maximum output row limit of 0"),
        std::string::npos);

    auto profile = conn->query("PROFILE UNWIND range(1, 3) AS x RETURN x", 1);
    ASSERT_TRUE(profile->isSuccess()) << profile->getErrorMessage();
    EXPECT_EQ(profile->getNumTuples(), 1);

    auto profileZero = conn->query("PROFILE UNWIND range(1, 3) AS x RETURN x", 0);
    ASSERT_FALSE(profileZero->isSuccess());
    EXPECT_NE(profileZero->getErrorMessage().find("maximum output row limit of 0"),
        std::string::npos);

    auto simpleSinkZero = conn->query(
        "CREATE NODE TABLE root_sink_policy(ID INT64, PRIMARY KEY(ID))", 0);
    ASSERT_FALSE(simpleSinkZero->isSuccess());
    EXPECT_NE(simpleSinkZero->getErrorMessage().find(
                  "only supported for side-effect-free read-only"),
        std::string::npos);

    ASSERT_TRUE(conn->query("BEGIN TRANSACTION")->isSuccess());
    ASSERT_TRUE(conn->query("ROLLBACK")->isSuccess());

    auto rolledBack = conn->query("MATCH (n:root_sink_policy) RETURN n");
    EXPECT_FALSE(rolledBack->isSuccess());

    auto createAfterRollback =
        conn->query("CREATE NODE TABLE root_sink_policy(ID INT64, PRIMARY KEY(ID))");
    ASSERT_TRUE(createAfterRollback->isSuccess()) << createAfterRollback->getErrorMessage();
    EXPECT_EQ(createAfterRollback->getNumTuples(), 1);

    auto transactionControl = conn->query("BEGIN TRANSACTION", 0);
    ASSERT_FALSE(transactionControl->isSuccess());
    EXPECT_NE(transactionControl->getErrorMessage().find(
                  "only supported for side-effect-free read-only"),
        std::string::npos);
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION")->isSuccess());
    ASSERT_TRUE(conn->query("ROLLBACK")->isSuccess());
}

TEST_F(ApiTest, PreparedMaxOutputRowsRejectsWritesBeforeSideEffects) {
    createPolicyTestSchema(*conn);
    auto prepared = conn->prepare("CREATE (:person {ID: $id, fName: 'blocked'})");
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    ASSERT_FALSE(prepared->isReadOnly());

    auto rejected = conn->executeWithParams(prepared.get(), {}, 1);
    ASSERT_FALSE(rejected->isSuccess());
    EXPECT_NE(rejected->getErrorMessage().find("only supported for side-effect-free read-only"),
        std::string::npos);

    auto count = conn->query("MATCH (n:person) WHERE n.ID = 999999 RETURN count(*)");
    ASSERT_TRUE(count->isSuccess()) << count->getErrorMessage();
    ASSERT_TRUE(count->hasNext());
    EXPECT_EQ(count->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(ApiTest, CappedExportIsRejectedBeforeExternalSideEffects) {
    const auto exportPath = TestHelper::getTempDir("capped_export_policy") + "/exported";
    auto rejected = conn->query("EXPORT DATABASE '" + exportPath + "'", 1);
    ASSERT_FALSE(rejected->isSuccess());
    EXPECT_NE(rejected->getErrorMessage().find("only supported for side-effect-free read-only"),
        std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(exportPath));

    const auto profileExportPath =
        TestHelper::getTempDir("capped_profile_export_policy") + "/exported";
    auto rejectedProfile =
        conn->query("PROFILE EXPORT DATABASE '" + profileExportPath + "'", 1);
    ASSERT_FALSE(rejectedProfile->isSuccess());
    EXPECT_NE(rejectedProfile->getErrorMessage().find(
                  "only supported for side-effect-free read-only"),
        std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(profileExportPath));
}

TEST_F(ApiTest, CappedCommitIsRejectedBeforeCommittingAnActiveTransaction) {
    createPolicyTestSchema(*conn);
    auto observer = std::make_unique<lbug::main::Connection>(database.get());

    ASSERT_TRUE(conn->query("BEGIN TRANSACTION")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:person {ID: 888888, fName: 'pending'})")->isSuccess());

    auto rejectedCommit = conn->query("PROFILE COMMIT", 0);
    ASSERT_FALSE(rejectedCommit->isSuccess());
    EXPECT_NE(rejectedCommit->getErrorMessage().find(
                  "only supported for side-effect-free read-only"),
        std::string::npos);

    auto localCount =
        conn->query("MATCH (n:person) WHERE n.ID = 888888 RETURN count(*)", 1);
    ASSERT_TRUE(localCount->isSuccess()) << localCount->getErrorMessage();
    EXPECT_EQ(localCount->getNext()->getValue(0)->getValue<int64_t>(), 1);

    auto externalCount =
        observer->query("MATCH (n:person) WHERE n.ID = 888888 RETURN count(*)");
    ASSERT_TRUE(externalCount->isSuccess()) << externalCount->getErrorMessage();
    EXPECT_EQ(externalCount->getNext()->getValue(0)->getValue<int64_t>(), 0);

    ASSERT_TRUE(conn->query("ROLLBACK")->isSuccess());
    auto afterRollback =
        observer->query("MATCH (n:person) WHERE n.ID = 888888 RETURN count(*)");
    ASSERT_TRUE(afterRollback->isSuccess()) << afterRollback->getErrorMessage();
    EXPECT_EQ(afterRollback->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(ApiTest, PreparedExecutionMaxOutputRowsIsNotSticky) {
    auto prepared = conn->prepare("UNWIND range(1, $count) AS x RETURN x");
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();

    std::unordered_map<std::string, std::unique_ptr<Value>> limitedParams;
    limitedParams.emplace("count", std::make_unique<Value>(int64_t{3}));
    auto limited = conn->executeWithParams(prepared.get(), std::move(limitedParams), 2);
    ASSERT_FALSE(limited->isSuccess());
    EXPECT_NE(limited->getErrorMessage().find("maximum output row limit of 2"),
        std::string::npos);

    auto unlimited = conn->execute(prepared.get(), std::make_pair(std::string{"count"}, int64_t{3}));
    ASSERT_TRUE(unlimited->isSuccess()) << unlimited->getErrorMessage();
    EXPECT_EQ(unlimited->getNumTuples(), 3);
}
