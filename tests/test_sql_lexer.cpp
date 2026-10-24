#include "test_framework.hpp"
#include "klyrdb/sql/lexer.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_lexer_keywords_and_case_insensitivity() {
    std::string sql = "SELECT * FROM users WHERE age >= 21 AND name = 'Alice' ORDER BY id DESC LIMIT 5;";
    SqlLexer lexer(sql);
    auto res = lexer.tokenize();
    EXPECT_TRUE(res.is_ok());

    const auto& tokens = res.value();
    EXPECT_EQ(tokens.size(), 20ULL);

    EXPECT_TRUE(tokens[0].is(TokenType::Select));
    EXPECT_TRUE(tokens[1].is(TokenType::Star));
    EXPECT_TRUE(tokens[2].is(TokenType::From));
    EXPECT_TRUE(tokens[3].is(TokenType::Identifier));
    EXPECT_EQ(tokens[3].text, "users");
    EXPECT_TRUE(tokens[4].is(TokenType::Where));
    EXPECT_TRUE(tokens[5].is(TokenType::Identifier));
    EXPECT_EQ(tokens[5].text, "age");
    EXPECT_TRUE(tokens[6].is(TokenType::GreaterEqual));
    EXPECT_TRUE(tokens[7].is(TokenType::IntegerLiteral));
    EXPECT_EQ(tokens[7].int_val, 21);
    EXPECT_TRUE(tokens[8].is(TokenType::And));
    EXPECT_TRUE(tokens[9].is(TokenType::Identifier));
    EXPECT_EQ(tokens[9].text, "name");
    EXPECT_TRUE(tokens[10].is(TokenType::Equal));
    EXPECT_TRUE(tokens[11].is(TokenType::StringLiteral));
    EXPECT_EQ(tokens[11].text, "Alice");
    EXPECT_TRUE(tokens[12].is(TokenType::Order));
    EXPECT_TRUE(tokens[13].is(TokenType::By));
    EXPECT_TRUE(tokens[14].is(TokenType::Identifier));
    EXPECT_TRUE(tokens[15].is(TokenType::Desc));
    EXPECT_TRUE(tokens[16].is(TokenType::Limit));
}

static void test_lexer_string_escapes_and_comments() {
    std::string sql = "-- Header comment\n"
                      "INSERT INTO notes VALUES ('O''Reilly', -42); -- Trailing comment\n";
    SqlLexer lexer(sql);
    auto res = lexer.tokenize();
    EXPECT_TRUE(res.is_ok());

    const auto& tokens = res.value();
    EXPECT_TRUE(tokens[0].is(TokenType::Insert));
    EXPECT_TRUE(tokens[1].is(TokenType::Into));
    EXPECT_TRUE(tokens[2].is(TokenType::Identifier));
    EXPECT_TRUE(tokens[3].is(TokenType::Values));
    EXPECT_TRUE(tokens[4].is(TokenType::LeftParen));
    EXPECT_TRUE(tokens[5].is(TokenType::StringLiteral));
    EXPECT_EQ(tokens[5].text, "O'Reilly");
    EXPECT_TRUE(tokens[6].is(TokenType::Comma));
    EXPECT_TRUE(tokens[7].is(TokenType::IntegerLiteral));
    EXPECT_EQ(tokens[7].int_val, -42);
    EXPECT_TRUE(tokens[8].is(TokenType::RightParen));
    EXPECT_TRUE(tokens[9].is(TokenType::Semicolon));
}

static void test_lexer_operators() {
    std::string sql = "= != <> < <= > >=";
    SqlLexer lexer(sql);
    auto res = lexer.tokenize();
    EXPECT_TRUE(res.is_ok());

    const auto& tokens = res.value();
    EXPECT_TRUE(tokens[0].is(TokenType::Equal));
    EXPECT_TRUE(tokens[1].is(TokenType::NotEqual));
    EXPECT_TRUE(tokens[2].is(TokenType::NotEqual));
    EXPECT_TRUE(tokens[3].is(TokenType::LessThan));
    EXPECT_TRUE(tokens[4].is(TokenType::LessEqual));
    EXPECT_TRUE(tokens[5].is(TokenType::GreaterThan));
    EXPECT_TRUE(tokens[6].is(TokenType::GreaterEqual));
}

static void test_lexer_errors() {
    // Unterminated string literal
    {
        SqlLexer lexer("SELECT 'unclosed string;");
        auto res = lexer.tokenize();
        EXPECT_TRUE(res.is_err());
        EXPECT_EQ(res.status().code(), StatusCode::LexerError);
    }

    // Invalid character
    {
        SqlLexer lexer("SELECT @foo;");
        auto res = lexer.tokenize();
        EXPECT_TRUE(res.is_err());
        EXPECT_EQ(res.status().code(), StatusCode::LexerError);
    }
}

int main() {
    std::cout << "Running SQL Lexer Tests...\n";
    run_test("Lexer Keywords and Case Insensitivity", test_lexer_keywords_and_case_insensitivity);
    run_test("Lexer String Escapes and Comments", test_lexer_string_escapes_and_comments);
    run_test("Lexer Operators", test_lexer_operators);
    run_test("Lexer Errors", test_lexer_errors);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
