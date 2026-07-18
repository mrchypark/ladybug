#pragma once

#define LBUG_QUERY_METADATA_ABI_VERSION 1

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/api.h"
#include "common/enums/statement_type.h"
#include "common/types/value/value.h"
#include "query_summary.h"

namespace lbug {
namespace common {
class LogicalType;
}
namespace parser {
class Statement;
}
namespace binder {
class Expression;
}
namespace planner {
class LogicalPlan;
}

namespace main {

class PreparedStatementMetadataAnalyzer;

class LBUG_API PreparedStatementMetadata {
    friend class ClientContext;
    friend class PreparedStatementMetadataAnalyzer;

public:
    const std::vector<std::string>& getParameterNames() const { return parameterNames; }
    const std::vector<std::string>& getIdentifiers() const { return identifiers; }
    const std::vector<std::string>& getNodeLabels() const { return nodeLabels; }
    const std::vector<std::string>& getRelLabels() const { return relLabels; }

    bool hasUnlabeledNodePattern() const { return unlabeledNodePattern; }
    bool hasUnlabeledRelPattern() const { return unlabeledRelPattern; }
    bool hasUnion() const { return union_; }
    bool hasUnionAll() const { return unionAll; }
    bool hasOptionalMatch() const { return optionalMatch; }
    bool hasUnwind() const { return unwind; }
    bool hasLoadFrom() const { return loadFrom; }
    bool hasInQueryCall() const { return inQueryCall; }
    bool hasRecursiveRelPattern() const { return recursiveRelPattern; }

    common::StatementType getStatementType() const { return statementType; }
    bool isReadOnly() const { return readOnly; }

private:
    std::vector<std::string> parameterNames;
    std::vector<std::string> identifiers;
    std::vector<std::string> nodeLabels;
    std::vector<std::string> relLabels;
    bool unlabeledNodePattern = false;
    bool unlabeledRelPattern = false;
    bool union_ = false;
    bool unionAll = false;
    bool optionalMatch = false;
    bool unwind = false;
    bool loadFrom = false;
    bool inQueryCall = false;
    bool recursiveRelPattern = false;
    common::StatementType statementType = common::StatementType::QUERY;
    bool readOnly = true;
};

// Prepared statement cached in client context and NEVER serialized to client side.
struct CachedPreparedStatement {
    bool useInternalCatalogEntry = false;
    std::shared_ptr<parser::Statement> parsedStatement;
    std::unique_ptr<planner::LogicalPlan> logicalPlan;
    std::vector<std::shared_ptr<binder::Expression>> columns;
    std::vector<std::string> columnNames;

    CachedPreparedStatement();
    ~CachedPreparedStatement();

    std::vector<std::string> getColumnNames() const;
    std::vector<common::LogicalType> getColumnTypes() const;
};

/**
 * @brief A prepared statement is a parameterized query which can avoid planning the same query for
 * repeated execution.
 */
class PreparedStatement {
    friend class Connection;
    friend class ClientContext;

public:
    LBUG_API ~PreparedStatement();
    /**
     * @return the query is prepared successfully or not.
     */
    LBUG_API bool isSuccess() const;
    /**
     * @return the error message if the query is not prepared successfully.
     */
    LBUG_API std::string getErrorMessage() const;
    /**
     * @return the prepared statement is read-only or not.
     */
    LBUG_API bool isReadOnly() const;

    const std::unordered_set<std::string>& getUnknownParameters() const {
        return unknownParameters;
    }
    bool canReuseCachedPlanWith(
        const std::unordered_map<std::string, std::unique_ptr<common::Value>>& inputParams) const;
    std::unordered_set<std::string> getKnownParameters();
    void updateParameter(const std::string& name, common::Value* value);
    void addParameter(const std::string& name, common::Value* value);
    LBUG_API void setParameter(const std::string& name, common::Value value);

    std::string getName() const { return cachedPreparedStatementName; }

    LBUG_API common::StatementType getStatementType() const;

    LBUG_API const PreparedStatementMetadata& getMetadata() const;

    static std::unique_ptr<PreparedStatement> getPreparedStatementWithError(
        const std::string& errorMessage);

private:
    bool success = true;
    bool readOnly = true;
    std::string errMsg;
    PreparedSummary preparedSummary;
    std::string cachedPreparedStatementName;
    std::unordered_set<std::string> unknownParameters;
    std::unordered_map<std::string, std::shared_ptr<common::Value>> parameterMap;
    PreparedStatementMetadata metadata;
};

} // namespace main
} // namespace lbug
