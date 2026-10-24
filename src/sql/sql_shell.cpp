#include "klyrdb/sql/sql_shell.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include <iomanip>
#include <sstream>

namespace klyrdb {

void SqlShell::run() {
    out_ << "=========================================================\n";
    out_ << " KlyrDB Interactive SQL Shell (Phase 5 - WAL & Txns)\n";
    out_ << " Type SQL statements ending with ';' or '.help' for help.\n";
    out_ << " Type '.quit' to exit.\n";
    out_ << "=========================================================\n\n";

    std::string current_query;
    std::string line;

    while (true) {
        if (current_query.empty()) {
            if (executor_.transaction_manager() != nullptr &&
                executor_.transaction_manager()->current_transaction() != nullptr) {
                out_ << "klyrdb(txn)> ";
            } else {
                out_ << "klyrdb> ";
            }
        } else {
            out_ << "   ...> ";
        }
        out_.flush();

        if (!std::getline(in_, line)) {
            out_ << "\nBye!\n";
            break;
        }

        // Trim leading whitespace for dot commands
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }

        std::string trimmed = line.substr(start);

        if (current_query.empty() && trimmed.front() == '.') {
            if (!execute_line(trimmed)) {
                out_ << "Bye!\n";
                break;
            }
            continue;
        }

        current_query += (current_query.empty() ? "" : " ") + trimmed;

        // Check if query ends with semicolon
        if (current_query.back() == ';') {
            execute_line(current_query);
            current_query.clear();
        }
    }
}

bool SqlShell::execute_line(const std::string& input) {
    if (input.empty()) return true;

    if (input == ".quit" || input == ".exit") {
        return false;
    }

    if (input == ".help") {
        print_help();
        return true;
    }

    if (input == ".tables") {
        print_tables();
        return true;
    }

    if (input.starts_with(".schema")) {
        std::string arg;
        if (input.size() > 7) {
            size_t pos = input.find_first_not_of(" \t", 7);
            if (pos != std::string::npos) {
                arg = input.substr(pos);
            }
        }
        print_schema(arg);
        return true;
    }

    if (input.starts_with(".indexes")) {
        std::string arg;
        if (input.size() > 8) {
            size_t pos = input.find_first_not_of(" \t", 8);
            if (pos != std::string::npos) {
                arg = input.substr(pos);
            }
        }
        print_indexes(arg);
        return true;
    }

    // Execute SQL statement
    auto res = executor_.execute(input);
    if (res.is_err()) {
        out_ << "Error: " << res.status().message() << "\n";
    } else {
        print_result(res.value(), out_);
    }

    return true;
}

void SqlShell::print_result(const QueryResult& result, std::ostream& out) {
    if (result.is_message()) {
        out << result.message() << "\n";
        return;
    }

    if (result.is_count()) {
        out << result.message() << "\n";
        return;
    }

    // Rows output: ASCII Table
    const auto& col_names = result.column_names();
    const auto& rows = result.rows();

    if (col_names.empty()) {
        out << "0 rows in set.\n";
        return;
    }

    // Calculate column widths
    std::vector<size_t> widths(col_names.size());
    for (size_t i = 0; i < col_names.size(); ++i) {
        widths[i] = col_names[i].size();
    }

    for (const auto& row : rows) {
        for (size_t i = 0; i < row.column_count() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row.get(i).to_string().size());
        }
    }

    // Lambda to print separator: +------+-----+
    auto print_separator = [&]() {
        out << "+";
        for (size_t w : widths) {
            out << std::string(w + 2, '-') << "+";
        }
        out << "\n";
    };

    print_separator();

    // Print headers
    out << "|";
    for (size_t i = 0; i < col_names.size(); ++i) {
        out << " " << std::left << std::setw(static_cast<int>(widths[i])) << col_names[i] << " |";
    }
    out << "\n";

    print_separator();

    // Print data rows
    for (const auto& row : rows) {
        out << "|";
        for (size_t i = 0; i < widths.size(); ++i) {
            std::string cell = (i < row.column_count()) ? row.get(i).to_string() : "";
            out << " " << std::left << std::setw(static_cast<int>(widths[i])) << cell << " |";
        }
        out << "\n";
    }

    print_separator();

    out << rows.size() << " " << (rows.size() == 1 ? "row" : "rows") << " in set.\n";
}

void SqlShell::print_help() {
    out_ << "\nAvailable Dot Commands:\n";
    out_ << "  .help                 Show this help menu\n";
    out_ << "  .tables               List all tables in the database\n";
    out_ << "  .schema [table]       Show schema for table (or all tables)\n";
    out_ << "  .indexes [table]      Show indexes for table (or all tables)\n";
    out_ << "  .quit / .exit         Exit the KlyrDB SQL shell\n\n";
    out_ << "Supported SQL Statements:\n";
    out_ << "  BEGIN;\n";
    out_ << "  COMMIT;\n";
    out_ << "  ROLLBACK;\n";
    out_ << "  CREATE TABLE tbl (col1 INTEGER, col2 TEXT);\n";
    out_ << "  CREATE INDEX idx ON tbl (col);\n";
    out_ << "  DROP TABLE tbl;\n";
    out_ << "  DROP INDEX idx;\n";
    out_ << "  INSERT INTO tbl [(col1, col2)] VALUES (val1, 'val2');\n";
    out_ << "  SELECT * | col1, col2 FROM tbl [WHERE col = val AND ...] [ORDER BY col [ASC|DESC]] [LIMIT n];\n";
    out_ << "  UPDATE tbl SET col1 = val1 WHERE col2 = val2;\n";
    out_ << "  DELETE FROM tbl WHERE col = val;\n\n";
}

void SqlShell::print_tables() {
    auto tables = executor_.table_catalog().list_tables();
    if (tables.empty()) {
        out_ << "No tables found in database.\n";
        return;
    }

    out_ << "\nTables in database:\n";
    for (const auto& tbl : tables) {
        out_ << "  - " << tbl.name << " (" << tbl.columns.size() << " columns, "
            << tbl.indexes.size() << " indexes)\n";
    }
    out_ << "\n";
}

void SqlShell::print_schema(std::string_view table_name) {
    auto tables = executor_.table_catalog().list_tables();
    if (tables.empty()) {
        out_ << "No tables found.\n";
        return;
    }

    std::string lower = canonicalize_name(table_name);
    bool found = false;

    for (const auto& tbl : tables) {
        if (!lower.empty() && tbl.name != lower) {
            continue;
        }
        found = true;
        out_ << "CREATE TABLE " << tbl.name << " (\n";
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            out_ << "    " << tbl.columns[i].name << " " << to_string(tbl.columns[i].type);
            if (i + 1 < tbl.columns.size()) {
                out_ << ",";
            }
            out_ << "\n";
        }
        out_ << ");\n";

        for (const auto& idx : tbl.indexes) {
            out_ << "CREATE INDEX " << idx.index_name << " ON " << tbl.name << " ("
                 << idx.column_name << ");\n";
        }
        out_ << "\n";
    }

    if (!found) {
        out_ << "Table '" << table_name << "' not found.\n";
    }
}

void SqlShell::print_indexes(std::string_view table_name) {
    auto tables = executor_.table_catalog().list_tables();
    std::string lower = canonicalize_name(table_name);
    bool found_any = false;

    for (const auto& tbl : tables) {
        if (!lower.empty() && tbl.name != lower) {
            continue;
        }
        if (tbl.indexes.empty()) continue;

        found_any = true;
        out_ << "Indexes on table '" << tbl.name << "':\n";
        for (const auto& idx : tbl.indexes) {
            out_ << "  - " << idx.index_name << " on column '" << idx.column_name
                 << "' (root page: " << idx.root_page_id << ")\n";
        }
    }

    if (!found_any) {
        out_ << "No indexes found" << (lower.empty() ? "." : " on specified table.") << "\n";
    }
}

} // namespace klyrdb
