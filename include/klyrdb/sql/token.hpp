#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace klyrdb {

enum class TokenType {
    // Keywords
    Create,
    Table,
    Index,
    On,
    Drop,
    Insert,
    Into,
    Values,
    Select,
    From,
    Where,
    Order,
    By,
    Asc,
    Desc,
    Limit,
    Update,
    Set,
    Delete,
    And,
    Begin,
    Commit,
    Rollback,
    TypeInteger,
    TypeText,
    Null,
    Not,
    Primary,
    Key,
    Unique,
    Foreign,
    References,
    Explain,
    Plan,
    Query,
    Analyze,

    // Identifiers & Literals
    Identifier,
    IntegerLiteral,
    StringLiteral,

    // Operators & Punctuation
    Comma,
    Semicolon,
    LeftParen,
    RightParen,
    Star,
    Equal,
    NotEqual,
    LessThan,
    LessEqual,
    GreaterThan,
    GreaterEqual,

    EndOfFile,
    Invalid
};

[[nodiscard]] inline std::string to_string(TokenType type) {
    switch (type) {
        case TokenType::Create:         return "CREATE";
        case TokenType::Table:          return "TABLE";
        case TokenType::Index:          return "INDEX";
        case TokenType::On:             return "ON";
        case TokenType::Drop:           return "DROP";
        case TokenType::Insert:         return "INSERT";
        case TokenType::Into:           return "INTO";
        case TokenType::Values:         return "VALUES";
        case TokenType::Select:         return "SELECT";
        case TokenType::From:           return "FROM";
        case TokenType::Where:          return "WHERE";
        case TokenType::Order:          return "ORDER";
        case TokenType::By:             return "BY";
        case TokenType::Asc:            return "ASC";
        case TokenType::Desc:           return "DESC";
        case TokenType::Limit:          return "LIMIT";
        case TokenType::Update:         return "UPDATE";
        case TokenType::Set:            return "SET";
        case TokenType::Delete:         return "DELETE";
        case TokenType::And:            return "AND";
        case TokenType::Begin:          return "BEGIN";
        case TokenType::Commit:         return "COMMIT";
        case TokenType::Rollback:       return "ROLLBACK";
        case TokenType::TypeInteger:    return "INTEGER";
        case TokenType::TypeText:       return "TEXT";
        case TokenType::Null:           return "NULL";
        case TokenType::Not:            return "NOT";
        case TokenType::Primary:        return "PRIMARY";
        case TokenType::Key:            return "KEY";
        case TokenType::Unique:         return "UNIQUE";
        case TokenType::Foreign:        return "FOREIGN";
        case TokenType::References:     return "REFERENCES";
        case TokenType::Explain:        return "EXPLAIN";
        case TokenType::Plan:           return "PLAN";
        case TokenType::Query:          return "QUERY";
        case TokenType::Analyze:        return "ANALYZE";
        case TokenType::Identifier:     return "Identifier";
        case TokenType::IntegerLiteral: return "IntegerLiteral";
        case TokenType::StringLiteral:  return "StringLiteral";
        case TokenType::Comma:          return ",";
        case TokenType::Semicolon:      return ";";
        case TokenType::LeftParen:      return "(";
        case TokenType::RightParen:     return ")";
        case TokenType::Star:           return "*";
        case TokenType::Equal:          return "=";
        case TokenType::NotEqual:       return "!=";
        case TokenType::LessThan:       return "<";
        case TokenType::LessEqual:      return "<=";
        case TokenType::GreaterThan:    return ">";
        case TokenType::GreaterEqual:   return ">=";
        case TokenType::EndOfFile:      return "EOF";
        case TokenType::Invalid:        return "Invalid";
    }
    return "Unknown";
}

struct Token {
    TokenType type{TokenType::Invalid};
    std::string text;
    int64_t int_val{0};
    size_t line{1};
    size_t column{1};

    [[nodiscard]] bool is(TokenType expected) const noexcept { return type == expected; }
};

} // namespace klyrdb
