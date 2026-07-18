#pragma once

#include <string>

#include "parser/expression/parsed_expression.h"

namespace lbug {
namespace parser {

struct PatternLabel {
    std::string name;
    std::vector<std::string> components;
};

class NodePattern {
protected:
    struct PatternLabelInfoTag {};

    NodePattern(std::string name, std::vector<PatternLabel> tableNames,
        std::vector<s_parsed_expr_pair> propertyKeyVals, PatternLabelInfoTag)
        : variableName{std::move(name)}, tableNameInfos{std::move(tableNames)},
          propertyKeyVals{std::move(propertyKeyVals)} {
        this->tableNames.reserve(tableNameInfos.size());
        for (const auto& tableName : tableNameInfos) {
            this->tableNames.push_back(tableName.name);
        }
    }

public:
    NodePattern(std::string name, std::vector<std::string> tableNames,
        std::vector<s_parsed_expr_pair> propertyKeyVals)
        : variableName{std::move(name)}, tableNames{std::move(tableNames)},
          propertyKeyVals{std::move(propertyKeyVals)} {
        tableNameInfos.reserve(this->tableNames.size());
        for (const auto& tableName : this->tableNames) {
            tableNameInfos.push_back({tableName, {tableName}});
        }
    }

    static NodePattern fromLabelInfos(std::string name, std::vector<PatternLabel> tableNames,
        std::vector<s_parsed_expr_pair> propertyKeyVals) {
        return NodePattern{std::move(name), std::move(tableNames), std::move(propertyKeyVals),
            PatternLabelInfoTag{}};
    }
    DELETE_COPY_DEFAULT_MOVE(NodePattern);

    virtual ~NodePattern() = default;

    inline std::string getVariableName() const { return variableName; }

    inline std::vector<std::string> getTableNames() const { return tableNames; }

    inline const std::vector<PatternLabel>& getTableNameInfos() const { return tableNameInfos; }

    inline const std::vector<s_parsed_expr_pair>& getPropertyKeyVals() const {
        return propertyKeyVals;
    }

protected:
    std::string variableName;
    std::vector<std::string> tableNames;
    std::vector<PatternLabel> tableNameInfos;
    std::vector<s_parsed_expr_pair> propertyKeyVals;
};

} // namespace parser
} // namespace lbug
