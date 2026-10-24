#pragma once

#include "klyrdb/sql/executor.hpp"
#include <iostream>
#include <string>

namespace klyrdb {

/// Interactive SQL REPL Shell with ASCII table formatting and dot commands.
class SqlShell {
public:
    explicit SqlShell(SqlExecutor& executor,
                      std::istream& in = std::cin,
                      std::ostream& out = std::cout) noexcept
        : executor_(executor), in_(in), out_(out) {}

    /// Runs the interactive REPL loop until EOF or .quit.
    void run();

    /// Executes a single SQL line or dot-command, printing the formatted output.
    bool execute_line(const std::string& line);

    /// Formats a QueryResult as a neat ASCII table or summary message.
    static void print_result(const QueryResult& result, std::ostream& out);

private:
    void print_help();
    void print_tables();
    void print_schema(std::string_view table_name = "");
    void print_indexes(std::string_view table_name = "");

    SqlExecutor& executor_;
    std::istream& in_;
    std::ostream& out_;
};

} // namespace klyrdb
