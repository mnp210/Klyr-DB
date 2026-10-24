#include "klyrdb/sql/parser.hpp"
#include "klyrdb/sql/lexer.hpp"
#include <charconv>

namespace klyrdb {

Result<std::unique_ptr<Statement>> SqlParser::parse_sql(std::string_view sql) {
    SqlLexer lexer(sql);
    auto tok_res = lexer.tokenize();
    if (tok_res.is_err()) {
        return tok_res.status();
    }
    SqlParser parser(std::move(tok_res.value()));
    return parser.parse_statement();
}

const Token& SqlParser::peek() const noexcept {
    if (cursor_ >= tokens_.size()) {
        return tokens_.back(); // EndOfFile
    }
    return tokens_[cursor_];
}

const Token& SqlParser::peek_prev() const noexcept {
    if (cursor_ == 0) return tokens_.front();
    return tokens_[cursor_ - 1];
}

bool SqlParser::is_at_end() const noexcept {
    return peek().is(TokenType::EndOfFile);
}

const Token& SqlParser::advance() noexcept {
    if (!is_at_end()) {
        cursor_++;
    }
    return peek_prev();
}

bool SqlParser::check(TokenType type) const noexcept {
    return peek().is(type);
}

bool SqlParser::match(TokenType type) noexcept {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

Result<Token> SqlParser::consume(TokenType type, std::string_view err_msg) {
    if (check(type)) {
        return advance();
    }
    const auto& tok = peek();
    return Status::syntax_error(std::string(err_msg) + " at line " +
                                std::to_string(tok.line) + ", col " + std::to_string(tok.column) +
                                " (found '" + tok.text + "')");
}

Result<std::unique_ptr<Statement>> SqlParser::parse_statement() {
    if (is_at_end()) {
        return Status::syntax_error("Empty SQL query");
    }

    std::unique_ptr<Statement> stmt;

    if (match(TokenType::Create)) {
        auto res = parse_create();
        if (res.is_err()) return res.status();
        stmt = std::move(res.value());
    } else if (match(TokenType::Drop)) {
        auto res = parse_drop();
        if (res.is_err()) return res.status();
        stmt = std::move(res.value());
    } else if (match(TokenType::Insert)) {
        auto res = parse_insert();
        if (res.is_err()) return res.status();
        stmt = std::move(res.value());
    } else if (match(TokenType::Select)) {
        auto res = parse_select();
        if (res.is_err()) return res.status();
        stmt = std::move(res.value());
    } else if (match(TokenType::Update)) {
        auto res = parse_update();
        if (res.is_err()) return res.status();
        stmt = std::move(res.value());
    } else if (match(TokenType::Delete)) {
        auto res = parse_delete();
        if (res.is_err()) return res.status();
        stmt = std::move(res.value());
    } else if (match(TokenType::Begin)) {
        if (!is_at_end() && (peek().text == "transaction" || peek().text == "TRANSACTION" ||
                             peek().text == "work" || peek().text == "WORK")) {
            advance();
        }
        stmt = std::make_unique<BeginStatement>();
    } else if (match(TokenType::Commit)) {
        if (!is_at_end() && (peek().text == "transaction" || peek().text == "TRANSACTION" ||
                             peek().text == "work" || peek().text == "WORK")) {
            advance();
        }
        stmt = std::make_unique<CommitStatement>();
    } else if (match(TokenType::Rollback)) {
        if (!is_at_end() && (peek().text == "transaction" || peek().text == "TRANSACTION" ||
                             peek().text == "work" || peek().text == "WORK")) {
            advance();
        }
        stmt = std::make_unique<RollbackStatement>();
    } else if (match(TokenType::Explain)) {
        auto res = parse_explain();
        if (res.is_err()) return res.status();
        stmt = std::move(res.value());
    } else if (match(TokenType::Analyze)) {
        auto analyze_stmt = std::make_unique<AnalyzeStatement>();
        if (!is_at_end() && !check(TokenType::Semicolon)) {
            auto id_res = consume(TokenType::Identifier, "Expected table name after ANALYZE");
            if (id_res.is_err()) return id_res.status();
            analyze_stmt->table_name = id_res.value().text;
        }
        stmt = std::move(analyze_stmt);
    } else {
        const auto& tok = peek();
        return Status::syntax_error("Unexpected token '" + tok.text + "' at line " +
                                    std::to_string(tok.line) + ", col " + std::to_string(tok.column));
    }

    // Optional trailing semicolon
    match(TokenType::Semicolon);

    // Verify no trailing extra tokens
    if (!is_at_end() && !peek().is(TokenType::Semicolon)) {
        const auto& tok = peek();
        return Status::syntax_error("Unexpected token after end of statement: '" + tok.text +
                                    "' at line " + std::to_string(tok.line) + ", col " +
                                    std::to_string(tok.column));
    }

    return stmt;
}

Result<std::unique_ptr<Statement>> SqlParser::parse_create() {
    if (match(TokenType::Table)) {
        return parse_create_table();
    }
    if (match(TokenType::Index)) {
        return parse_create_index();
    }
    return Status::syntax_error("Expected TABLE or INDEX after CREATE");
}

Result<std::unique_ptr<Statement>> SqlParser::parse_create_table() {
    auto name_tok = consume(TokenType::Identifier, "Expected table name after CREATE TABLE");
    if (name_tok.is_err()) return name_tok.status();

    auto lparen = consume(TokenType::LeftParen, "Expected '(' after table name");
    if (lparen.is_err()) return lparen.status();

    auto stmt = std::make_unique<CreateTableStatement>();
    stmt->table_name = name_tok.value().text;

    while (true) {
        // 1. Table-level PRIMARY KEY constraint: PRIMARY KEY (col)
        if (match(TokenType::Primary)) {
            auto key_tok = consume(TokenType::Key, "Expected 'KEY' after 'PRIMARY'");
            if (key_tok.is_err()) return key_tok.status();

            auto lp = consume(TokenType::LeftParen, "Expected '(' after PRIMARY KEY");
            if (lp.is_err()) return lp.status();

            auto col_tok = consume(TokenType::Identifier, "Expected column name in PRIMARY KEY");
            if (col_tok.is_err()) return col_tok.status();

            if (check(TokenType::Comma)) {
                return Status::unsupported_sql("Composite primary keys are not supported");
            }

            auto rp = consume(TokenType::RightParen, "Expected ')' after PRIMARY KEY column");
            if (rp.is_err()) return rp.status();

            TableConstraint tc;
            tc.type = ConstraintType::PrimaryKey;
            tc.column_name = col_tok.value().text;
            stmt->constraints.push_back(std::move(tc));
        }
        // 2. Table-level UNIQUE constraint: UNIQUE (col)
        else if (match(TokenType::Unique)) {
            auto lp = consume(TokenType::LeftParen, "Expected '(' after UNIQUE");
            if (lp.is_err()) return lp.status();

            auto col_tok = consume(TokenType::Identifier, "Expected column name in UNIQUE");
            if (col_tok.is_err()) return col_tok.status();

            if (check(TokenType::Comma)) {
                return Status::unsupported_sql("Composite unique constraints are not supported");
            }

            auto rp = consume(TokenType::RightParen, "Expected ')' after UNIQUE column");
            if (rp.is_err()) return rp.status();

            TableConstraint tc;
            tc.type = ConstraintType::Unique;
            tc.column_name = col_tok.value().text;
            stmt->constraints.push_back(std::move(tc));
        }
        // 3. Table-level FOREIGN KEY constraint: FOREIGN KEY (col) REFERENCES ref_table(ref_col)
        else if (match(TokenType::Foreign)) {
            auto key_tok = consume(TokenType::Key, "Expected 'KEY' after 'FOREIGN'");
            if (key_tok.is_err()) return key_tok.status();

            auto lp1 = consume(TokenType::LeftParen, "Expected '(' after FOREIGN KEY");
            if (lp1.is_err()) return lp1.status();

            auto col_tok = consume(TokenType::Identifier, "Expected column name in FOREIGN KEY");
            if (col_tok.is_err()) return col_tok.status();

            if (check(TokenType::Comma)) {
                return Status::unsupported_sql("Composite foreign keys are not supported");
            }

            auto rp1 = consume(TokenType::RightParen, "Expected ')' after FOREIGN KEY column");
            if (rp1.is_err()) return rp1.status();

            auto ref_tok = consume(TokenType::References, "Expected 'REFERENCES' in FOREIGN KEY");
            if (ref_tok.is_err()) return ref_tok.status();

            auto ref_tbl_tok = consume(TokenType::Identifier, "Expected referenced table name");
            if (ref_tbl_tok.is_err()) return ref_tbl_tok.status();

            auto lp2 = consume(TokenType::LeftParen, "Expected '(' after referenced table name");
            if (lp2.is_err()) return lp2.status();

            auto ref_col_tok = consume(TokenType::Identifier, "Expected referenced column name");
            if (ref_col_tok.is_err()) return ref_col_tok.status();

            if (check(TokenType::Comma)) {
                return Status::unsupported_sql("Composite foreign keys are not supported");
            }

            auto rp2 = consume(TokenType::RightParen, "Expected ')' after referenced column name");
            if (rp2.is_err()) return rp2.status();

            TableConstraint tc;
            tc.type = ConstraintType::ForeignKey;
            tc.column_name = col_tok.value().text;
            tc.ref_table_name = ref_tbl_tok.value().text;
            tc.ref_column_name = ref_col_tok.value().text;
            stmt->constraints.push_back(std::move(tc));
        }
        // 4. Column definition with optional column-level constraints
        else {
            auto col_name_tok = consume(TokenType::Identifier, "Expected column name");
            if (col_name_tok.is_err()) return col_name_tok.status();

            SqlType col_type = SqlType::Integer;
            if (match(TokenType::TypeInteger)) {
                col_type = SqlType::Integer;
            } else if (match(TokenType::TypeText)) {
                col_type = SqlType::Text;
            } else if (peek().is(TokenType::Identifier)) {
                // Check if user wrote VARCHAR, STRING, etc.
                auto type_res = parse_sql_type(advance().text);
                if (type_res.is_err()) return type_res.status();
                col_type = type_res.value();
            } else {
                return Status::syntax_error("Expected data type (INTEGER or TEXT) for column '" +
                                            col_name_tok.value().text + "'");
            }

            stmt->columns.push_back(ColumnDef{col_name_tok.value().text, col_type});

            // Parse optional column-level constraints
            while (true) {
                if (match(TokenType::Not)) {
                    auto null_tok = consume(TokenType::Null, "Expected 'NULL' after 'NOT'");
                    if (null_tok.is_err()) return null_tok.status();

                    TableConstraint tc;
                    tc.type = ConstraintType::NotNull;
                    tc.column_name = col_name_tok.value().text;
                    stmt->constraints.push_back(std::move(tc));
                } else if (match(TokenType::Primary)) {
                    auto key_tok = consume(TokenType::Key, "Expected 'KEY' after 'PRIMARY'");
                    if (key_tok.is_err()) return key_tok.status();

                    TableConstraint tc;
                    tc.type = ConstraintType::PrimaryKey;
                    tc.column_name = col_name_tok.value().text;
                    stmt->constraints.push_back(std::move(tc));
                } else if (match(TokenType::Unique)) {
                    TableConstraint tc;
                    tc.type = ConstraintType::Unique;
                    tc.column_name = col_name_tok.value().text;
                    stmt->constraints.push_back(std::move(tc));
                } else if (match(TokenType::References)) {
                    auto ref_tbl_tok = consume(TokenType::Identifier, "Expected referenced table name after 'REFERENCES'");
                    if (ref_tbl_tok.is_err()) return ref_tbl_tok.status();

                    auto lp = consume(TokenType::LeftParen, "Expected '(' after referenced table name");
                    if (lp.is_err()) return lp.status();

                    auto ref_col_tok = consume(TokenType::Identifier, "Expected referenced column name");
                    if (ref_col_tok.is_err()) return ref_col_tok.status();

                    if (check(TokenType::Comma)) {
                        return Status::unsupported_sql("Composite foreign keys are not supported");
                    }

                    auto rp = consume(TokenType::RightParen, "Expected ')' after referenced column name");
                    if (rp.is_err()) return rp.status();

                    TableConstraint tc;
                    tc.type = ConstraintType::ForeignKey;
                    tc.column_name = col_name_tok.value().text;
                    tc.ref_table_name = ref_tbl_tok.value().text;
                    tc.ref_column_name = ref_col_tok.value().text;
                    stmt->constraints.push_back(std::move(tc));
                } else {
                    break;
                }
            }
        }

        if (match(TokenType::Comma)) {
            continue;
        }
        break;
    }

    auto rparen = consume(TokenType::RightParen, "Expected ')' after column definitions");
    if (rparen.is_err()) return rparen.status();

    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<std::unique_ptr<Statement>> SqlParser::parse_create_index() {
    auto idx_name_tok = consume(TokenType::Identifier, "Expected index name after CREATE INDEX");
    if (idx_name_tok.is_err()) return idx_name_tok.status();

    auto on_tok = consume(TokenType::On, "Expected 'ON' after index name");
    if (on_tok.is_err()) return on_tok.status();

    auto tbl_name_tok = consume(TokenType::Identifier, "Expected table name after ON");
    if (tbl_name_tok.is_err()) return tbl_name_tok.status();

    auto lparen = consume(TokenType::LeftParen, "Expected '(' before column name in CREATE INDEX");
    if (lparen.is_err()) return lparen.status();

    auto col_name_tok = consume(TokenType::Identifier, "Expected column name for index");
    if (col_name_tok.is_err()) return col_name_tok.status();

    auto rparen = consume(TokenType::RightParen, "Expected ')' after column name in CREATE INDEX");
    if (rparen.is_err()) return rparen.status();

    auto stmt = std::make_unique<CreateIndexStatement>();
    stmt->index_name = idx_name_tok.value().text;
    stmt->table_name = tbl_name_tok.value().text;
    stmt->column_name = col_name_tok.value().text;

    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<std::unique_ptr<Statement>> SqlParser::parse_drop() {
    if (match(TokenType::Table)) {
        return parse_drop_table();
    }
    if (match(TokenType::Index)) {
        return parse_drop_index();
    }
    return Status::syntax_error("Expected TABLE or INDEX after DROP");
}

Result<std::unique_ptr<Statement>> SqlParser::parse_drop_table() {
    auto tbl_name_tok = consume(TokenType::Identifier, "Expected table name after DROP TABLE");
    if (tbl_name_tok.is_err()) return tbl_name_tok.status();

    auto stmt = std::make_unique<DropTableStatement>();
    stmt->table_name = tbl_name_tok.value().text;
    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<std::unique_ptr<Statement>> SqlParser::parse_drop_index() {
    auto idx_name_tok = consume(TokenType::Identifier, "Expected index name after DROP INDEX");
    if (idx_name_tok.is_err()) return idx_name_tok.status();

    auto stmt = std::make_unique<DropIndexStatement>();
    stmt->index_name = idx_name_tok.value().text;

    if (match(TokenType::On)) {
        auto tbl_name_tok = consume(TokenType::Identifier, "Expected table name after ON");
        if (tbl_name_tok.is_err()) return tbl_name_tok.status();
        stmt->table_name = tbl_name_tok.value().text;
    }

    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<std::unique_ptr<Statement>> SqlParser::parse_insert() {
    auto into_tok = consume(TokenType::Into, "Expected 'INTO' after INSERT");
    if (into_tok.is_err()) return into_tok.status();

    auto tbl_name_tok = consume(TokenType::Identifier, "Expected table name after INSERT INTO");
    if (tbl_name_tok.is_err()) return tbl_name_tok.status();

    auto stmt = std::make_unique<InsertStatement>();
    stmt->table_name = tbl_name_tok.value().text;

    // Optional column list: INSERT INTO tbl (c1, c2) ...
    if (match(TokenType::LeftParen)) {
        while (true) {
            auto col_tok = consume(TokenType::Identifier, "Expected column name in column list");
            if (col_tok.is_err()) return col_tok.status();
            stmt->column_names.push_back(col_tok.value().text);

            if (match(TokenType::Comma)) {
                continue;
            }
            break;
        }
        auto rparen = consume(TokenType::RightParen, "Expected ')' after column list");
        if (rparen.is_err()) return rparen.status();
    }

    auto val_tok = consume(TokenType::Values, "Expected 'VALUES' in INSERT statement");
    if (val_tok.is_err()) return val_tok.status();

    auto lparen = consume(TokenType::LeftParen, "Expected '(' after VALUES");
    if (lparen.is_err()) return lparen.status();

    while (true) {
        auto val_res = parse_literal();
        if (val_res.is_err()) return val_res.status();
        stmt->values.push_back(std::move(val_res.value()));

        if (match(TokenType::Comma)) {
            continue;
        }
        break;
    }

    auto rparen = consume(TokenType::RightParen, "Expected ')' after value list");
    if (rparen.is_err()) return rparen.status();

    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<std::unique_ptr<Statement>> SqlParser::parse_select() {
    auto stmt = std::make_unique<SelectStatement>();

    if (match(TokenType::Star)) {
        stmt->select_all = true;
    } else {
        stmt->select_all = false;
        while (true) {
            auto col_tok = consume(TokenType::Identifier, "Expected column name in SELECT list");
            if (col_tok.is_err()) return col_tok.status();
            stmt->column_names.push_back(col_tok.value().text);

            if (match(TokenType::Comma)) {
                continue;
            }
            break;
        }
    }

    auto from_tok = consume(TokenType::From, "Expected 'FROM' after SELECT projection");
    if (from_tok.is_err()) return from_tok.status();

    auto tbl_name_tok = consume(TokenType::Identifier, "Expected table name after FROM");
    if (tbl_name_tok.is_err()) return tbl_name_tok.status();
    stmt->table_name = tbl_name_tok.value().text;

    // Optional WHERE clause
    if (match(TokenType::Where)) {
        auto where_res = parse_where_clause();
        if (where_res.is_err()) return where_res.status();
        stmt->where = std::move(where_res.value());
    }

    // Optional ORDER BY clause
    if (match(TokenType::Order)) {
        auto by_tok = consume(TokenType::By, "Expected 'BY' after ORDER");
        if (by_tok.is_err()) return by_tok.status();

        auto col_tok = consume(TokenType::Identifier, "Expected column name after ORDER BY");
        if (col_tok.is_err()) return col_tok.status();

        bool ascending = true;
        if (match(TokenType::Asc)) {
            ascending = true;
        } else if (match(TokenType::Desc)) {
            ascending = false;
        }

        stmt->order_by = OrderByClause{col_tok.value().text, ascending};
    }

    // Optional LIMIT clause
    if (match(TokenType::Limit)) {
        auto lim_tok = consume(TokenType::IntegerLiteral, "Expected non-negative integer after LIMIT");
        if (lim_tok.is_err()) return lim_tok.status();
        if (lim_tok.value().int_val < 0) {
            return Status::invalid_argument("LIMIT value cannot be negative");
        }
        stmt->limit.has_limit = true;
        stmt->limit.limit = static_cast<size_t>(lim_tok.value().int_val);
    }

    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<std::unique_ptr<Statement>> SqlParser::parse_update() {
    auto tbl_name_tok = consume(TokenType::Identifier, "Expected table name after UPDATE");
    if (tbl_name_tok.is_err()) return tbl_name_tok.status();

    auto stmt = std::make_unique<UpdateStatement>();
    stmt->table_name = tbl_name_tok.value().text;

    auto set_tok = consume(TokenType::Set, "Expected 'SET' in UPDATE statement");
    if (set_tok.is_err()) return set_tok.status();

    while (true) {
        auto col_tok = consume(TokenType::Identifier, "Expected column name in SET clause");
        if (col_tok.is_err()) return col_tok.status();

        auto eq_tok = consume(TokenType::Equal, "Expected '=' after column name in SET clause");
        if (eq_tok.is_err()) return eq_tok.status();

        auto val_res = parse_literal();
        if (val_res.is_err()) return val_res.status();

        stmt->set_clauses.push_back(SetClause{col_tok.value().text, std::move(val_res.value())});

        if (match(TokenType::Comma)) {
            continue;
        }
        break;
    }

    // Safety check: WHERE is mandatory for UPDATE
    if (!match(TokenType::Where)) {
        return Status::constraint_violation("UPDATE requires a WHERE clause for safety");
    }

    auto where_res = parse_where_clause();
    if (where_res.is_err()) return where_res.status();
    stmt->where = std::move(where_res.value());

    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<std::unique_ptr<Statement>> SqlParser::parse_delete() {
    auto from_tok = consume(TokenType::From, "Expected 'FROM' after DELETE");
    if (from_tok.is_err()) return from_tok.status();

    auto tbl_name_tok = consume(TokenType::Identifier, "Expected table name after DELETE FROM");
    if (tbl_name_tok.is_err()) return tbl_name_tok.status();

    auto stmt = std::make_unique<DeleteStatement>();
    stmt->table_name = tbl_name_tok.value().text;

    // Safety check: WHERE is mandatory for DELETE
    if (!match(TokenType::Where)) {
        return Status::constraint_violation("DELETE requires a WHERE clause for safety");
    }

    auto where_res = parse_where_clause();
    if (where_res.is_err()) return where_res.status();
    stmt->where = std::move(where_res.value());

    return std::unique_ptr<Statement>(std::move(stmt));
}

Result<WhereClause> SqlParser::parse_where_clause() {
    WhereClause clause;
    while (true) {
        auto pred_res = parse_predicate();
        if (pred_res.is_err()) return pred_res.status();
        clause.predicates.push_back(std::move(pred_res.value()));

        if (match(TokenType::And)) {
            continue;
        }
        break;
    }
    return clause;
}

Result<BinaryPredicate> SqlParser::parse_predicate() {
    // Check if left is column name or literal
    if (check(TokenType::Identifier)) {
        auto col_name = advance().text;

        BinaryOp op = BinaryOp::Equal;
        if (match(TokenType::Equal))             op = BinaryOp::Equal;
        else if (match(TokenType::NotEqual))     op = BinaryOp::NotEqual;
        else if (match(TokenType::LessThan))     op = BinaryOp::LessThan;
        else if (match(TokenType::LessEqual))    op = BinaryOp::LessEqual;
        else if (match(TokenType::GreaterThan))  op = BinaryOp::GreaterThan;
        else if (match(TokenType::GreaterEqual)) op = BinaryOp::GreaterEqual;
        else {
            return Status::syntax_error("Expected comparison operator (=, !=, <, <=, >, >=) in WHERE predicate");
        }

        auto val_res = parse_literal();
        if (val_res.is_err()) return val_res.status();

        return BinaryPredicate{col_name, op, std::move(val_res.value())};
    } else {
        // Left is literal, right is column name: e.g. 5 < age -> age > 5
        auto val_res = parse_literal();
        if (val_res.is_err()) return val_res.status();

        BinaryOp op = BinaryOp::Equal;
        if (match(TokenType::Equal))             op = BinaryOp::Equal;
        else if (match(TokenType::NotEqual))     op = BinaryOp::NotEqual;
        else if (match(TokenType::LessThan))     op = BinaryOp::GreaterThan;
        else if (match(TokenType::LessEqual))    op = BinaryOp::GreaterEqual;
        else if (match(TokenType::GreaterThan))  op = BinaryOp::LessThan;
        else if (match(TokenType::GreaterEqual)) op = BinaryOp::LessEqual;
        else {
            return Status::syntax_error("Expected comparison operator in WHERE predicate");
        }

        auto col_tok = consume(TokenType::Identifier, "Expected column name in WHERE predicate");
        if (col_tok.is_err()) return col_tok.status();

        return BinaryPredicate{col_tok.value().text, op, std::move(val_res.value())};
    }
}

Result<SqlValue> SqlParser::parse_literal() {
    if (match(TokenType::Null)) {
        return SqlValue::null();
    }
    if (match(TokenType::IntegerLiteral)) {
        return SqlValue(peek_prev().int_val);
    }
    if (match(TokenType::StringLiteral)) {
        return SqlValue(peek_prev().text);
    }
    const auto& tok = peek();
    return Status::syntax_error("Expected literal value (null, integer or text) at line " +
                                std::to_string(tok.line) + ", col " + std::to_string(tok.column));
}


Result<std::unique_ptr<Statement>> SqlParser::parse_explain() {
    auto explain_stmt = std::make_unique<ExplainStatement>();
    if (match(TokenType::Analyze)) {
        explain_stmt->is_analyze = true;
    } else if (match(TokenType::Query)) {
        if (!match(TokenType::Plan)) {
            return Status::syntax_error("Expected 'PLAN' after 'EXPLAIN QUERY'");
        }
        explain_stmt->is_query_plan = true;
    } else if (match(TokenType::Plan)) {
        explain_stmt->is_query_plan = true;
    }

    if (match(TokenType::Select)) {
        auto select_res = parse_select();
        if (select_res.is_err()) return select_res.status();
        explain_stmt->query = std::move(select_res.value());
    } else {
        return Status::unsupported_sql("EXPLAIN is only supported for read-only queries (SELECT)");
    }

    return std::unique_ptr<Statement>(std::move(explain_stmt));
}

} // namespace klyrdb
