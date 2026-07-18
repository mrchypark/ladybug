#pragma once

#include "main/prepared_statement.h"
#include "parser/expression/parsed_expression_visitor.h"
#include "parser/parsed_statement_visitor.h"

namespace lbug {
namespace parser {
struct BaseScanSource;
class PatternElement;
class ProjectionBody;
class ReadingClause;
class SingleQuery;
class UpdatingClause;
struct JoinHintNode;
} // namespace parser

namespace main {

class PreparedStatementMetadataAnalyzer final : private parser::StatementVisitor,
                                                private parser::ParsedExpressionVisitor {
public:
    static PreparedStatementMetadata analyze(const parser::Statement& statement);

private:
    void visitQuery(const parser::Statement& statement) override;
    void visitSwitch(const parser::ParsedExpression* expression) override;
    void visitFunctionExpr(const parser::ParsedExpression* expression) override;
    void visitPropertyExpr(const parser::ParsedExpression* expression) override;
    void visitVariableExpr(const parser::ParsedExpression* expression) override;
    void visitParamExpr(const parser::ParsedExpression* expression) override;
    void visitSubqueryExpr(const parser::ParsedExpression* expression) override;
    void visitLambdaExpr(const parser::ParsedExpression* expression) override;

    void analyzeSingleQuery(const parser::SingleQuery& query);
    void analyzeReadingClause(const parser::ReadingClause& clause);
    void analyzeUpdatingClause(const parser::UpdatingClause& clause);
    void visitProjectionBody(const parser::ProjectionBody& projectionBody);
    void visitPatternElements(const std::vector<parser::PatternElement>& patternElements);
    void visitJoinHint(const std::shared_ptr<parser::JoinHintNode>& hint);
    void visitScanSource(const parser::BaseScanSource& source);
    void visitExpression(const parser::ParsedExpression* expression);
    void addIdentifier(const std::string& identifier);
    static void sortAndDeduplicate(std::vector<std::string>& values);

private:
    PreparedStatementMetadata metadata;
};

} // namespace main
} // namespace lbug
