#include "test_framework.hpp"
#include "klyrdb/sql/parser.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_parser_create_table() {
    auto res = SqlParser::parse_sql("CREATE TABLE users (id INTEGER, name TEXT, age INTEGER);");
    EXPECT_TRUE(res.is_ok());

    auto stmt = std::move(res.value());
    EXPECT_EQ(stmt->type(), StatementType::CreateTable);

    auto ct = static_cast<CreateTableStatement*>(stmt.get());
    EXPECT_EQ(ct->table_name, "users");
    EXPECT_EQ(ct->columns.size(), 3ULL);
    EXPECT_EQ(ct->columns[0].name, "id");
    EXPECT_EQ(ct->columns[0].type, SqlType::Integer);
    EXPECT_EQ(ct->columns[1].name, "name");
    EXPECT_EQ(ct->columns[1].type, SqlType::Text);
    EXPECT_EQ(ct->columns[2].name, "age");
    EXPECT_EQ(ct->columns[2].type, SqlType::Integer);
}

static void test_parser_create_index() {
    auto res = SqlParser::parse_sql("CREATE INDEX idx_user_id ON users (id);");
    EXPECT_TRUE(res.is_ok());

    auto stmt = std::move(res.value());
    EXPECT_EQ(stmt->type(), StatementType::CreateIndex);

    auto ci = static_cast<CreateIndexStatement*>(stmt.get());
    EXPECT_EQ(ci->index_name, "idx_user_id");
    EXPECT_EQ(ci->table_name, "users");
    EXPECT_EQ(ci->column_name, "id");
}

static void test_parser_insert() {
    // Explicit column list
    {
        auto res = SqlParser::parse_sql("INSERT INTO products (id, name, price) VALUES (10, 'Book', 25);");
        EXPECT_TRUE(res.is_ok());

        auto stmt = std::move(res.value());
        EXPECT_EQ(stmt->type(), StatementType::Insert);
        auto ins = static_cast<InsertStatement*>(stmt.get());
        EXPECT_EQ(ins->table_name, "products");
        EXPECT_EQ(ins->column_names.size(), 3ULL);
        EXPECT_EQ(ins->values.size(), 3ULL);
        EXPECT_EQ(ins->values[0].as_integer(), 10);
        EXPECT_EQ(ins->values[1].as_text(), "Book");
        EXPECT_EQ(ins->values[2].as_integer(), 25);
    }

    // Positional insert
    {
        auto res = SqlParser::parse_sql("INSERT INTO products VALUES (20, 'Pen', 5);");
        EXPECT_TRUE(res.is_ok());
        auto stmt = std::move(res.value());
        auto ins = static_cast<InsertStatement*>(stmt.get());
        EXPECT_TRUE(ins->column_names.empty());
        EXPECT_EQ(ins->values.size(), 3ULL);
    }
}

static void test_parser_select() {
    auto res = SqlParser::parse_sql("SELECT name, price FROM products WHERE price >= 10 AND name != 'Draft' ORDER BY price DESC LIMIT 5;");
    EXPECT_TRUE(res.is_ok());

    auto stmt = std::move(res.value());
    EXPECT_EQ(stmt->type(), StatementType::Select);

    auto sel = static_cast<SelectStatement*>(stmt.get());
    EXPECT_FALSE(sel->select_all);
    EXPECT_EQ(sel->column_names.size(), 2ULL);
    EXPECT_EQ(sel->column_names[0], "name");
    EXPECT_EQ(sel->column_names[1], "price");
    EXPECT_EQ(sel->table_name, "products");

    EXPECT_EQ(sel->where.predicates.size(), 2ULL);
    EXPECT_EQ(sel->where.predicates[0].column_name, "price");
    EXPECT_EQ(sel->where.predicates[0].op, BinaryOp::GreaterEqual);
    EXPECT_EQ(sel->where.predicates[0].value.as_integer(), 10);
    EXPECT_EQ(sel->where.predicates[1].column_name, "name");
    EXPECT_EQ(sel->where.predicates[1].op, BinaryOp::NotEqual);
    EXPECT_EQ(sel->where.predicates[1].value.as_text(), "Draft");

    EXPECT_TRUE(sel->order_by.has_value());
    EXPECT_EQ(sel->order_by->column_name, "price");
    EXPECT_FALSE(sel->order_by->ascending);

    EXPECT_TRUE(sel->limit.has_limit);
    EXPECT_EQ(sel->limit.limit, 5ULL);
}

static void test_parser_update_and_delete_safety() {
    // Valid UPDATE with WHERE
    {
        auto res = SqlParser::parse_sql("UPDATE users SET age = 23, name = 'Alice Jr' WHERE id = 1;");
        EXPECT_TRUE(res.is_ok());
        auto stmt = std::move(res.value());
        EXPECT_EQ(stmt->type(), StatementType::Update);
    }

    // UPDATE without WHERE must fail
    {
        auto res = SqlParser::parse_sql("UPDATE users SET age = 23;");
        EXPECT_TRUE(res.is_err());
        EXPECT_EQ(res.status().code(), StatusCode::ConstraintViolation);
    }

    // Valid DELETE with WHERE
    {
        auto res = SqlParser::parse_sql("DELETE FROM users WHERE id = 1;");
        EXPECT_TRUE(res.is_ok());
        auto stmt = std::move(res.value());
        EXPECT_EQ(stmt->type(), StatementType::Delete);
    }

    // DELETE without WHERE must fail
    {
        auto res = SqlParser::parse_sql("DELETE FROM users;");
        EXPECT_TRUE(res.is_err());
        EXPECT_EQ(res.status().code(), StatusCode::ConstraintViolation);
    }
}

static void test_parser_syntax_errors() {
    // Missing table name
    {
        auto res = SqlParser::parse_sql("CREATE TABLE (id INTEGER);");
        EXPECT_TRUE(res.is_err());
        EXPECT_EQ(res.status().code(), StatusCode::SyntaxError);
    }

    // Missing FROM in SELECT
    {
        auto res = SqlParser::parse_sql("SELECT * users;");
        EXPECT_TRUE(res.is_err());
        EXPECT_EQ(res.status().code(), StatusCode::SyntaxError);
    }
}

int main() {
    std::cout << "Running SQL Parser Tests...\n";
    run_test("Parser Create Table", test_parser_create_table);
    run_test("Parser Create Index", test_parser_create_index);
    run_test("Parser Insert", test_parser_insert);
    run_test("Parser Select", test_parser_select);
    run_test("Parser Update and Delete Safety", test_parser_update_and_delete_safety);
    run_test("Parser Syntax Errors", test_parser_syntax_errors);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
