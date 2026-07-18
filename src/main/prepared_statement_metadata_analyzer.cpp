#include "main/prepared_statement_metadata_analyzer.h"

#include <algorithm>

#include "common/enums/clause_type.h"
#include "common/enums/query_rel_type.h"
#include "common/enums/scan_source_type.h"
#include "parser/expression/parsed_function_expression.h"
#include "parser/expression/parsed_lambda_expression.h"
#include "parser/expression/parsed_parameter_expression.h"
#include "parser/expression/parsed_property_expression.h"
#include "parser/expression/parsed_subquery_expression.h"
#include "parser/expression/parsed_variable_expression.h"
#include "parser/query/graph_pattern/pattern_element.h"
#include "parser/query/reading_clause/in_query_call_clause.h"
#include "parser/query/reading_clause/load_from.h"
#include "parser/query/reading_clause/match_clause.h"
#include "parser/query/reading_clause/unwind_clause.h"
#include "parser/query/regular_query.h"
#include "parser/query/return_with_clause/projection_body.h"
#include "parser/query/return_with_clause/with_clause.h"
#include "parser/query/updating_clause/delete_clause.h"
#include "parser/query/updating_clause/insert_clause.h"
#include "parser/query/updating_clause/merge_clause.h"
#include "parser/query/updating_clause/set_clause.h"
#include "parser/scan_source.h"

using namespace lbug::common;
using namespace lbug::parser;

namespace lbug {
namespace main {

PreparedStatementMetadata PreparedStatementMetadataAnalyzer::analyze(const Statement& statement) {
    PreparedStatementMetadataAnalyzer analyzer;
    analyzer.metadata.statementType = statement.getStatementType();
    analyzer.StatementVisitor::visit(statement);
    sortAndDeduplicate(analyzer.metadata.parameterNames);
    sortAndDeduplicate(analyzer.metadata.identifiers);
    sortAndDeduplicate(analyzer.metadata.nodeLabels);
    sortAndDeduplicate(analyzer.metadata.relLabels);
    return std::move(analyzer.metadata);
}

void PreparedStatementMetadataAnalyzer::visitQuery(const Statement& statement) {
    const auto& query = statement.constCast<RegularQuery>();
    metadata.union_ |= query.getNumSingleQueries() > 1;
    const auto unionTypes = query.getIsUnionAll();
    metadata.unionAll |= std::ranges::find(unionTypes, true) != unionTypes.end();
    for (auto i = 0u; i < query.getNumSingleQueries(); ++i) {
        analyzeSingleQuery(*query.getSingleQuery(i));
    }
}

void PreparedStatementMetadataAnalyzer::analyzeSingleQuery(const SingleQuery& query) {
    for (auto i = 0u; i < query.getNumQueryParts(); ++i) {
        const auto queryPart = query.getQueryPart(i);
        for (auto j = 0u; j < queryPart->getNumReadingClauses(); ++j) {
            analyzeReadingClause(*queryPart->getReadingClause(j));
        }
        for (auto j = 0u; j < queryPart->getNumUpdatingClauses(); ++j) {
            analyzeUpdatingClause(*queryPart->getUpdatingClause(j));
        }
        const auto withClause = queryPart->getWithClause();
        visitProjectionBody(*withClause->getProjectionBody());
        if (withClause->hasWhereExpression()) {
            visitExpression(withClause->getWhereExpression());
        }
    }
    for (auto i = 0u; i < query.getNumReadingClauses(); ++i) {
        analyzeReadingClause(*query.getReadingClause(i));
    }
    for (auto i = 0u; i < query.getNumUpdatingClauses(); ++i) {
        analyzeUpdatingClause(*query.getUpdatingClause(i));
    }
    if (query.hasReturnClause()) {
        visitProjectionBody(*query.getReturnClause()->getProjectionBody());
    }
}

void PreparedStatementMetadataAnalyzer::analyzeReadingClause(const ReadingClause& clause) {
    if (clause.hasWherePredicate()) {
        visitExpression(clause.getWherePredicate());
    }
    switch (clause.getClauseType()) {
    case ClauseType::MATCH: {
        const auto& match = clause.constCast<MatchClause>();
        metadata.optionalMatch |= match.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH;
        visitPatternElements(match.getPatternElementsRef());
        if (match.hasHint()) {
            visitJoinHint(match.getHint());
        }
    } break;
    case ClauseType::UNWIND: {
        const auto& unwind = clause.constCast<UnwindClause>();
        metadata.unwind = true;
        addIdentifier(unwind.getAlias());
        visitExpression(unwind.getExpression());
    } break;
    case ClauseType::IN_QUERY_CALL: {
        const auto& call = clause.constCast<InQueryCallClause>();
        metadata.inQueryCall = true;
        visitExpression(call.getFunctionExpression());
        for (const auto& variable : call.getYieldVariables()) {
            addIdentifier(variable.name);
            addIdentifier(variable.alias);
        }
    } break;
    case ClauseType::LOAD_FROM: {
        const auto& load = clause.constCast<LoadFrom>();
        metadata.loadFrom = true;
        visitScanSource(*load.getSource());
        for (const auto& definition : load.getColumnDefinitions()) {
            addIdentifier(definition.name);
        }
        for (const auto& [name, expression] : load.getParsingOptions()) {
            addIdentifier(name);
            visitExpression(expression.get());
        }
    } break;
    default:
        break;
    }
}

void PreparedStatementMetadataAnalyzer::analyzeUpdatingClause(const UpdatingClause& clause) {
    switch (clause.getClauseType()) {
    case ClauseType::SET: {
        for (const auto& [left, right] : clause.constCast<SetClause>().getSetItemsRef()) {
            visitExpression(left.get());
            visitExpression(right.get());
        }
    } break;
    case ClauseType::DELETE_: {
        const auto& deleteClause = clause.constCast<DeleteClause>();
        for (auto i = 0u; i < deleteClause.getNumExpressions(); ++i) {
            visitExpression(deleteClause.getExpression(i));
        }
    } break;
    case ClauseType::INSERT: {
        visitPatternElements(clause.constCast<InsertClause>().getPatternElementsRef());
    } break;
    case ClauseType::MERGE: {
        const auto& merge = clause.constCast<MergeClause>();
        visitPatternElements(merge.getPatternElementsRef());
        for (const auto& [left, right] : merge.getOnMatchSetItemsRef()) {
            visitExpression(left.get());
            visitExpression(right.get());
        }
        for (const auto& [left, right] : merge.getOnCreateSetItemsRef()) {
            visitExpression(left.get());
            visitExpression(right.get());
        }
    } break;
    default:
        break;
    }
}

void PreparedStatementMetadataAnalyzer::visitProjectionBody(const ProjectionBody& projectionBody) {
    for (const auto& expression : projectionBody.getProjectionExpressions()) {
        visitExpression(expression.get());
    }
    for (const auto& expression : projectionBody.getOrderByExpressions()) {
        visitExpression(expression.get());
    }
    if (projectionBody.hasSkipExpression()) {
        visitExpression(projectionBody.getSkipExpression());
    }
    if (projectionBody.hasLimitExpression()) {
        visitExpression(projectionBody.getLimitExpression());
    }
}

void PreparedStatementMetadataAnalyzer::visitPatternElements(
    const std::vector<PatternElement>& patternElements) {
    const auto visitNode = [this](const NodePattern& node) {
        addIdentifier(node.getVariableName());
        const auto labels = node.getTableNameInfos();
        metadata.unlabeledNodePattern |= labels.empty();
        for (const auto& label : labels) {
            metadata.nodeLabels.push_back(label.name);
            addIdentifier(label.name);
            for (const auto& component : label.components) {
                addIdentifier(component);
            }
        }
        for (const auto& [name, expression] : node.getPropertyKeyVals()) {
            addIdentifier(name);
            visitExpression(expression.get());
        }
    };
    for (const auto& element : patternElements) {
        addIdentifier(element.getPathName());
        visitNode(*element.getFirstNodePattern());
        for (auto i = 0u; i < element.getNumPatternElementChains(); ++i) {
            const auto chain = element.getPatternElementChain(i);
            const auto rel = chain->getRelPattern();
            addIdentifier(rel->getVariableName());
            const auto labels = rel->getTableNameInfos();
            metadata.unlabeledRelPattern |= labels.empty();
            for (const auto& label : labels) {
                metadata.relLabels.push_back(label.name);
                addIdentifier(label.name);
                for (const auto& component : label.components) {
                    addIdentifier(component);
                }
            }
            for (const auto& [name, expression] : rel->getPropertyKeyVals()) {
                addIdentifier(name);
                visitExpression(expression.get());
            }
            metadata.recursiveRelPattern |= QueryRelTypeUtils::isRecursive(rel->getRelType());
            if (QueryRelTypeUtils::isRecursive(rel->getRelType())) {
                const auto info = rel->getRecursiveInfo();
                addIdentifier(info->weightPropertyName);
                addIdentifier(info->relName);
                addIdentifier(info->nodeName);
                visitExpression(info->whereExpression.get());
                for (const auto& expression : info->relProjectionList) {
                    visitExpression(expression.get());
                }
                for (const auto& expression : info->nodeProjectionList) {
                    visitExpression(expression.get());
                }
            }
            visitNode(*chain->getNodePattern());
        }
    }
}

void PreparedStatementMetadataAnalyzer::visitScanSource(const BaseScanSource& source) {
    switch (source.type) {
    case ScanSourceType::PARAM:
        visitExpression(source.constPtrCast<ParameterScanSource>()->paramExpression.get());
        break;
    case ScanSourceType::OBJECT:
        for (const auto& name : source.constPtrCast<ObjectScanSource>()->objectNames) {
            addIdentifier(name);
        }
        break;
    case ScanSourceType::QUERY:
        StatementVisitor::visit(*source.constPtrCast<QueryScanSource>()->statement);
        break;
    case ScanSourceType::TABLE_FUNC:
        visitExpression(source.constPtrCast<TableFuncScanSource>()->functionExpression.get());
        break;
    default:
        break;
    }
}

void PreparedStatementMetadataAnalyzer::visitJoinHint(const std::shared_ptr<JoinHintNode>& hint) {
    addIdentifier(hint->variableName);
    for (const auto& child : hint->children) {
        visitJoinHint(child);
    }
}

void PreparedStatementMetadataAnalyzer::visitExpression(const ParsedExpression* expression) {
    if (expression != nullptr) {
        ParsedExpressionVisitor::visit(expression);
    }
}

void PreparedStatementMetadataAnalyzer::visitSwitch(const ParsedExpression* expression) {
    if (expression->hasAlias()) {
        addIdentifier(expression->getAlias());
    }
    ParsedExpressionVisitor::visitSwitch(expression);
}

void PreparedStatementMetadataAnalyzer::visitFunctionExpr(const ParsedExpression* expression) {
    if (expression->getExpressionType() != ExpressionType::FUNCTION) {
        return;
    }
    const auto& function = expression->constCast<ParsedFunctionExpression>();
    addIdentifier(function.getFunctionName());
    for (const auto& argument : function.getOptionalArguments()) {
        addIdentifier(argument);
    }
}

void PreparedStatementMetadataAnalyzer::visitPropertyExpr(const ParsedExpression* expression) {
    addIdentifier(expression->constCast<ParsedPropertyExpression>().getPropertyName());
}

void PreparedStatementMetadataAnalyzer::visitVariableExpr(const ParsedExpression* expression) {
    addIdentifier(expression->constCast<ParsedVariableExpression>().getVariableName());
}

void PreparedStatementMetadataAnalyzer::visitParamExpr(const ParsedExpression* expression) {
    metadata.parameterNames.push_back(
        expression->constCast<ParsedParameterExpression>().getParameterName());
}

void PreparedStatementMetadataAnalyzer::visitSubqueryExpr(const ParsedExpression* expression) {
    const auto& subquery = expression->constCast<ParsedSubqueryExpression>();
    visitPatternElements(subquery.getPatternElements());
    if (subquery.hasHint()) {
        visitJoinHint(subquery.getHint());
    }
    if (subquery.hasWhereClause()) {
        visitExpression(subquery.getWhereClause());
    }
}

void PreparedStatementMetadataAnalyzer::visitLambdaExpr(const ParsedExpression* expression) {
    for (const auto& name : expression->constCast<ParsedLambdaExpression>().getVarNames()) {
        addIdentifier(name);
    }
}

void PreparedStatementMetadataAnalyzer::addIdentifier(const std::string& identifier) {
    if (!identifier.empty()) {
        metadata.identifiers.push_back(identifier);
    }
}

void PreparedStatementMetadataAnalyzer::sortAndDeduplicate(std::vector<std::string>& values) {
    std::ranges::sort(values);
    values.erase(std::ranges::unique(values).begin(), values.end());
}

} // namespace main
} // namespace lbug
