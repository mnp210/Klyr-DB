#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/sql/token.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace klyrdb {

/// Fast, robust lexical analyzer for KlyrDB SQL subset.
class SqlLexer {
public:
    explicit SqlLexer(std::string_view sql) noexcept
        : source_(sql), cursor_(0), line_(1), col_(1) {}

    /// Tokenizes the entire source string into a list of Tokens ending with EndOfFile.
    [[nodiscard]] Result<std::vector<Token>> tokenize();

    /// Scans and returns the next Token.
    [[nodiscard]] Result<Token> next_token();

private:
    [[nodiscard]] bool is_at_end() const noexcept { return cursor_ >= source_.size(); }
    [[nodiscard]] char peek() const noexcept { return is_at_end() ? '\0' : source_[cursor_]; }
    [[nodiscard]] char peek_next() const noexcept {
        return (cursor_ + 1 >= source_.size()) ? '\0' : source_[cursor_ + 1];
    }
    char advance() noexcept;
    void skip_whitespace_and_comments();

    [[nodiscard]] Result<Token> lex_identifier_or_keyword();
    [[nodiscard]] Result<Token> lex_number(bool is_negative = false);
    [[nodiscard]] Result<Token> lex_string_literal();

    std::string_view source_;
    size_t cursor_{0};
    size_t line_{1};
    size_t col_{1};
};

} // namespace klyrdb
