# KlyrDB Public C++ API Reference (v0.12.0)

## 1. Overview

KlyrDB provides a clean, modern C++20 public programming interface that encapsulates internal database components (page files, buffer pool frames, B+ tree leaf operations, WAL records, and table catalog encoding).

All public types and functions live under the `klyrdb` namespace and can be included via the master umbrella header:

```cpp
#include <klyrdb/klyrdb.hpp>
```

---

## 2. Core Concepts & Resource Lifetimes

### RAII & Ownership
- **`Database`**: Represents the database on disk. Owns the file descriptors, the buffer pool cache, write-ahead logging, and system catalogs. Closing or destroying a `Database` flushes dirty pages and releases OS process locks.
- **`Connection`**: Represents an active session executing queries against a `Database`. Tracks session state (such as active transactions).
- **`ResultSet`**: Immutable query result returned by value. Outlives the query that generated it and holds deep copies of rows, columns, and data values.
- **`TransactionGuard`**: RAII transaction manager. Begins a transaction upon creation, rolls back automatically on destruction unless `commit()` was explicitly executed.

---

## 3. API Reference

### 3.1 Version Inspection (`klyrdb/version.hpp`)

```cpp
namespace klyrdb {

struct Version {
    int major;
    int minor;
    int patch;
    std::string to_string() const;
};

Version version() noexcept;
constexpr uint32_t storage_format_version() noexcept;
constexpr uint32_t backup_format_version() noexcept;

}
```

### 3.2 Database Lifecycle (`klyrdb/database.hpp`)

```cpp
namespace klyrdb {

struct DatabaseOptions {
    size_t buffer_pool_size{64}; // Number of 4096-byte frames in cache
    bool wal_enabled{true};      // Enable write-ahead logging
    bool lock_process{true};     // Acquire exclusive OS process file lock
};

class Database {
public:
    static Result<std::unique_ptr<Database>> open(
        const std::filesystem::path& path,
        DatabaseOptions options = {}
    );

    Result<std::unique_ptr<Connection>> connect();
    Result<ResultSet> execute(std::string_view sql);

    Status checkpoint();
    Result<bool> integrity_check();
    void close();

    const std::filesystem::path& path() const noexcept;
    bool is_open() const noexcept;
};

}
```

### 3.3 Connections & Transactions (`klyrdb/connection.hpp`)

```cpp
namespace klyrdb {

class Connection {
public:
    Result<ResultSet> execute(std::string_view sql);

    Status begin_transaction();
    Status commit();
    Status rollback();

    bool is_in_transaction() const noexcept;
};

class TransactionGuard {
public:
    explicit TransactionGuard(Connection& conn);
    ~TransactionGuard(); // Automatically rolls back if active

    Status commit();
    Status rollback();
    bool is_active() const noexcept;
};

}
```

### 3.4 Results & Row Values (`klyrdb/result.hpp`)

```cpp
namespace klyrdb {

using Value = SqlValue;

class Row {
public:
    size_t size() const noexcept;
    bool empty() const noexcept;

    const Value& get(size_t index) const;
    const Value& operator[](size_t index) const;

    const Value& get(std::string_view col_name) const;
    const Value& operator[](std::string_view col_name) const;

    const std::vector<Value>& values() const noexcept;
};

class ResultSet {
public:
    bool is_rows() const noexcept;
    bool is_count() const noexcept;
    bool is_message() const noexcept;

    size_t row_count() const noexcept;
    size_t column_count() const noexcept;
    size_t rows_affected() const noexcept;
    const std::string& message() const noexcept;

    const std::vector<std::string>& column_names() const noexcept;
    const std::vector<Row>& rows() const noexcept;

    const Row& operator[](size_t index) const;

    auto begin() const noexcept;
    auto end() const noexcept;
};

}
```

---

## 4. Usage Example

```cpp
#include <klyrdb/klyrdb.hpp>
#include <iostream>

int main() {
    // 1. Open or create database
    auto db = klyrdb::Database::open("app.kdb").value();

    // 2. Establish connection
    auto conn = db->connect().value();

    // 3. Create table and insert rows
    (void)conn->execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    (void)conn->execute("INSERT INTO users VALUES (1, 'Alice');");

    // 4. Query data
    auto rs = conn->execute("SELECT id, name FROM users;").value();
    for (const auto& row : rs) {
        std::cout << row["id"].as_integer() << ": " << row["name"].as_text() << "\n";
    }

    // 5. Transaction with automatic rollback on error
    {
        klyrdb::TransactionGuard txn(*conn);
        (void)conn->execute("INSERT INTO users VALUES (2, 'Bob');");
        txn.commit();
    }

    return 0;
}
```
