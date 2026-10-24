#include "klyrdb/sql/lexer.hpp"
#include <cctype>
#include <charconv>

namespace klyrdb {

char SqlLexer::advance() noexcept {
    if (is_at_end()) return '\0';
    char c = source_[cursor_++];
    if (c == '\n') {
        line_++;
        col_ = 1;
    } else {
        col_++;
    }
    return c;
}

void SqlLexer::skip_whitespace_and_comments() {
    while (!is_at_end()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '-' && peek_next() == '-') {
            // SQL line comment
            advance();
            advance();
            while (!is_at_end() && peek() != '\n') {
                advance();
            }
        } else {
            break;
        }
    }
}

Result<std::vector<Token>> SqlLexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        auto tok_res = next_token();
        if (tok_res.is_err()) {
            return tok_res.status();
        }
        tokens.push_back(tok_res.value());
        if (tokens.back().is(TokenType::EndOfFile)) {
            break;
        }
    }
    return tokens;
}

Result<Token> SqlLexer::next_token() {
    skip_whitespace_and_comments();

    const size_t start_line = line_;
    const size_t start_col = col_;

    if (is_at_end()) {
        return Token{TokenType::EndOfFile, "", 0, start_line, start_col};
    }

    char c = peek();

    // Identifiers and keywords
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return lex_identifier_or_keyword();
    }

    // Numbers
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return lex_number(false);
    }

    // Negative numbers
    if (c == '-' && std::isdigit(static_cast<unsigned char>(peek_next()))) {
        advance(); // consume '-'
        return lex_number(true);
    }

    // String literals
    if (c == '\'') {
        return lex_string_literal();
    }

    // Punctuation and operators
    advance(); // consume first character

    switch (c) {
        case ',': return Token{TokenType::Comma, ",", 0, start_line, start_col};
        case ';': return Token{TokenType::Semicolon, ";", 0, start_line, start_col};
        case '(': return Token{TokenType::LeftParen, "(", 0, start_line, start_col};
        case ')': return Token{TokenType::RightParen, ")", 0, start_line, start_col};
        case '*': return Token{TokenType::Star, "*", 0, start_line, start_col};
        case '=': return Token{TokenType::Equal, "=", 0, start_line, start_col};

        case '!':
            if (peek() == '=') {
                advance();
                return Token{TokenType::NotEqual, "!=", 0, start_line, start_col};
            }
            return Status::lexer_error("Unexpected character '!' at line " + std::to_string(start_line) +
                                      ", col " + std::to_string(start_col));

        case '<':
            if (peek() == '=') {
                advance();
                return Token{TokenType::LessEqual, "<=", 0, start_line, start_col};
            }
            if (peek() == '>') {
                advance();
                return Token{TokenType::NotEqual, "<>", 0, start_line, start_col};
            }
            return Token{TokenType::LessThan, "<", 0, start_line, start_col};

        case '>':
            if (peek() == '=') {
                advance();
                return Token{TokenType::GreaterEqual, ">=", 0, start_line, start_col};
            }
            return Token{TokenType::GreaterThan, ">", 0, start_line, start_col};

        default:
            return Status::lexer_error("Unexpected character '" + std::string(1, c) + "' at line " +
                                      std::to_string(start_line) + ", col " + std::to_string(start_col));
    }
}

Result<Token> SqlLexer::lex_identifier_or_keyword() {
    const size_t start_line = line_;
    const size_t start_col = col_;
    size_t start_pos = cursor_;

    while (!is_at_end()) {
        char c = peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            advance();
        } else {
            break;
        }
    }

    std::string_view raw = source_.substr(start_pos, cursor_ - start_pos);
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    TokenType type = TokenType::Identifier;
    if (lower == "create")       type = TokenType::Create;
    else if (lower == "table")   type = TokenType::Table;
    else if (lower == "index")   type = TokenType::Index;
    else if (lower == "on")      type = TokenType::On;
    else if (lower == "drop")    type = TokenType::Drop;
    else if (lower == "insert")  type = TokenType::Insert;
    else if (lower == "into")    type = TokenType::Into;
    else if (lower == "values")  type = TokenType::Values;
    else if (lower == "select")  type = TokenType::Select;
    else if (lower == "from")    type = TokenType::From;
    else if (lower == "where")   type = TokenType::Where;
    else if (lower == "order")   type = TokenType::Order;
    else if (lower == "by")      type = TokenType::By;
    else if (lower == "asc")     type = TokenType::Asc;
    else if (lower == "desc")    type = TokenType::Desc;
    else if (lower == "limit")    type = TokenType::Limit;
    else if (lower == "update")   type = TokenType::Update;
    else if (lower == "set")      type = TokenType::Set;
    else if (lower == "delete")   type = TokenType::Delete;
    else if (lower == "and")      type = TokenType::And;
    else if (lower == "begin")    type = TokenType::Begin;
    else if (lower == "commit")   type = TokenType::Commit;
    else if (lower == "rollback") type = TokenType::Rollback;
    else if (lower == "integer")  type = TokenType::TypeInteger;
    else if (lower == "text")     type = TokenType::TypeText;
    else if (lower == "null")       type = TokenType::Null;
    else if (lower == "not")        type = TokenType::Not;
    else if (lower == "primary")    type = TokenType::Primary;
    else if (lower == "key")        type = TokenType::Key;
    else if (lower == "unique")     type = TokenType::Unique;
    else if (lower == "foreign")    type = TokenType::Foreign;
    else if (lower == "references") type = TokenType::References;
    else if (lower == "explain")    type = TokenType::Explain;
    else if (lower == "plan")       type = TokenType::Plan;
    else if (lower == "query")      type = TokenType::Query;
    else if (lower == "analyze")    type = TokenType::Analyze;

    return Token{type, std::string(raw), 0, start_line, start_col};
}

Result<Token> SqlLexer::lex_number(bool is_negative) {
    const size_t start_line = line_;
    const size_t start_col = is_negative ? col_ - 1 : col_;
    size_t start_pos = cursor_;

    while (!is_at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    std::string_view num_str = source_.substr(start_pos, cursor_ - start_pos);
    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val);
    if (ec != std::errc()) {
        return Status::lexer_error("Invalid or overflow integer literal at line " +
                                  std::to_string(start_line) + ", col " + std::to_string(start_col));
    }

    if (is_negative) {
        val = -val;
    }

    std::string full_str = (is_negative ? "-" : "") + std::string(num_str);
    return Token{TokenType::IntegerLiteral, std::move(full_str), val, start_line, start_col};
}

Result<Token> SqlLexer::lex_string_literal() {
    const size_t start_line = line_;
    const size_t start_col = col_;
    advance(); // consume opening quote

    std::string result;
    while (!is_at_end()) {
        char c = advance();
        if (c == '\'') {
            if (peek() == '\'') {
                // Escaped single quote: ''
                advance();
                result.push_back('\'');
            } else {
                // Closing quote
                return Token{TokenType::StringLiteral, std::move(result), 0, start_line, start_col};
            }
        } else {
            result.push_back(c);
        }
    }

    return Status::lexer_error("Unterminated string literal starting at line " +
                              std::to_string(start_line) + ", col " + std::to_string(start_col));
}

} // namespace klyrdb
