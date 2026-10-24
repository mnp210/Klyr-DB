#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/sql/token.hpp"
#include "klyrdb/sql/ast.hpp"
#include <string_view>
#include <vector>
#include <memory>

namespace klyrdb {

/// Recursive descent parser for the KlyrDB SQL subset.
class SqlParser {
public:
    explicit SqlParser(std::vector<Token> tokens) noexcept
        : tokens_(std::move(tokens)), cursor_(0) {}

    /// Convenience helper to tokenize and parse a SQL query string.
    [[nodiscard]] static Result<std::unique_ptr<Statement>> parse_sql(std::string_view sql);

    /// Parses the token stream into an AST Statement.
    [[nodiscard]] Result<std::unique_ptr<Statement>> parse_statement();

private:
    [[nodiscard]] const Token& peek() const noexcept;
    [[nodiscard]] const Token& peek_prev() const noexcept;
    [[nodiscard]] bool is_at_end() const noexcept;
    const Token& advance() noexcept;
    [[nodiscard]] bool check(TokenType type) const noexcept;
    bool match(TokenType type) noexcept;
    [[nodiscard]] Result<Token> consume(TokenType type, std::string_view err_msg);

    Result<std::unique_ptr<Statement>> parse_create();
    Result<std::unique_ptr<Statement>> parse_create_table();
    Result<std::unique_ptr<Statement>> parse_create_index();

    Result<std::unique_ptr<Statement>> parse_drop();
    Result<std::unique_ptr<Statement>> parse_drop_table();
    Result<std::unique_ptr<Statement>> parse_drop_index();

    Result<std::unique_ptr<Statement>> parse_insert();
    Result<std::unique_ptr<Statement>> parse_select();
    Result<std::unique_ptr<Statement>> parse_update();
    Result<std::unique_ptr<Statement>> parse_delete();
    Result<std::unique_ptr<Statement>> parse_explain();

    Result<WhereClause> parse_where_clause();
    Result<BinaryPredicate> parse_predicate();
    Result<SqlValue> parse_literal();

    std::vector<Token> tokens_;
    size_t cursor_{0};
};

} // namespace klyrdb
