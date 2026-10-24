#pragma once

#include "klyrdb/common/result.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/result.hpp"
#include "klyrdb/connection.hpp"
#include <filesystem>
#include <string_view>
#include <memory>

namespace klyrdb {

struct DatabaseOptions {
    size_t buffer_pool_size{64};
    bool wal_enabled{true};
    bool lock_process{true};
};

class DatabaseImpl;

/// Represents an opened KlyrDB database instance. Manages file handles, buffer pool, WAL, and catalogs.
class Database {
public:
    ~Database();

    /// Opens or creates a KlyrDB database at the specified path.
    [[nodiscard]] static Result<std::unique_ptr<Database>> open(
        const std::filesystem::path& path,
        DatabaseOptions options = {}
    );

    /// Creates a new connection session to execute queries against this database.
    [[nodiscard]] Result<std::unique_ptr<Connection>> connect();

    /// Convenience method to execute a SQL query directly against the database.
    [[nodiscard]] Result<ResultSet> execute(std::string_view sql);

    /// Flushes dirty pages and advances the checkpoint horizon safely.
    [[nodiscard]] Status checkpoint();

    /// Executes non-destructive storage and relational integrity checks.
    [[nodiscard]] Result<bool> integrity_check();

    /// Closes all database handles and flushes dirty state.
    void close();

    /// Returns the filesystem path of this database.
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    /// Returns true if the database is currently open.
    [[nodiscard]] bool is_open() const noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    /// Internal implementation accessor.
    [[nodiscard]] DatabaseImpl* impl() noexcept { return impl_.get(); }

private:
    explicit Database(std::unique_ptr<DatabaseImpl> impl);
    std::unique_ptr<DatabaseImpl> impl_;
};

} // namespace klyrdb
