#include "test_framework.hpp"
#include "klyrdb/klyrdb.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_api_version() {
    auto v = version();
    EXPECT_EQ(v.major, 0);
    EXPECT_EQ(v.minor, 12);
    EXPECT_EQ(v.patch, 0);
    EXPECT_EQ(v.to_string(), "0.12.0");

    EXPECT_EQ(storage_format_version(), 1U);
    EXPECT_EQ(backup_format_version(), 1U);
}

static void test_api_database_lifecycle_and_crud() {
    auto path = temp_test_file("test_public_api_crud");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    ScopedTempFile wal_cleanup(path.string() + ".wal");

    // 1. Open / create database
    {
        auto db_res = Database::open(path);
        EXPECT_TRUE(db_res.is_ok());
        auto db = std::move(db_res.value());
        EXPECT_TRUE(db->is_open());
        EXPECT_EQ(db->path(), path);

        // 2. Create table
        auto cr_res = db->execute("CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT, price INTEGER);");
        EXPECT_TRUE(cr_res.is_ok());
        EXPECT_TRUE(cr_res.value().is_message());

        // 3. Insert records
        auto ins1 = db->execute("INSERT INTO products VALUES (1, 'keyboard', 100);");
        EXPECT_TRUE(ins1.is_ok());
        EXPECT_TRUE(ins1.value().is_count());
        EXPECT_EQ(ins1.value().rows_affected(), 1ULL);

        auto ins2 = db->execute("INSERT INTO products VALUES (2, 'mouse', 50);");
        EXPECT_TRUE(ins2.is_ok());
        EXPECT_EQ(ins2.value().rows_affected(), 1ULL);

        // 4. Query via Connection
        auto conn_res = db->connect();
        EXPECT_TRUE(conn_res.is_ok());
        auto conn = std::move(conn_res.value());

        auto sel = conn->execute("SELECT id, name, price FROM products ORDER BY id ASC;");
        EXPECT_TRUE(sel.is_ok());
        auto rs = std::move(sel.value());

        EXPECT_TRUE(rs.is_rows());
        EXPECT_EQ(rs.row_count(), 2ULL);
        EXPECT_EQ(rs.column_count(), 3ULL);
        EXPECT_EQ(rs.column_names()[0], "id");
        EXPECT_EQ(rs.column_names()[1], "name");
        EXPECT_EQ(rs.column_names()[2], "price");

        // Row 0 access by index and column name
        const auto& r0 = rs[0];
        EXPECT_EQ(r0[0].as_integer(), 1);
        EXPECT_EQ(r0["name"].as_text(), "keyboard");
        EXPECT_EQ(r0["price"].as_integer(), 100);

        // Row 1 access
        const auto& r1 = rs[1];
        EXPECT_EQ(r1["id"].as_integer(), 2);
        EXPECT_EQ(r1["name"].as_text(), "mouse");
        EXPECT_EQ(r1["price"].as_integer(), 50);

        // Range-based for loop
        int count = 0;
        for (const auto& row : rs) {
            EXPECT_TRUE(row.size() == 3);
            count++;
        }
        EXPECT_EQ(count, 2);

        // Error handling on invalid column
        bool threw1 = false;
        try { (void)r0.get("nonexistent_col"); } catch (const std::out_of_range&) { threw1 = true; }
        EXPECT_TRUE(threw1);

        bool threw2 = false;
        try { (void)r0.get(999); } catch (const std::out_of_range&) { threw2 = true; }
        EXPECT_TRUE(threw2);

        // Syntax error reporting
        auto bad_query = conn->execute("SELECT * FROM;");
        EXPECT_TRUE(bad_query.is_err());
        EXPECT_TRUE(!bad_query.status().message().empty());

        // Checkpoint & Integrity
        EXPECT_TRUE(db->checkpoint().is_ok());
        auto integ = db->integrity_check();
        EXPECT_TRUE(integ.is_ok());
        EXPECT_TRUE(integ.value());

        db->close();
        EXPECT_FALSE(db->is_open());
    }

    // 5. Reopen and verify persistence
    {
        auto db_res = Database::open(path);
        EXPECT_TRUE(db_res.is_ok());
        auto db = std::move(db_res.value());
        EXPECT_TRUE(db->is_open());

        auto sel = db->execute("SELECT id, name, price FROM products WHERE id = 2;");
        EXPECT_TRUE(sel.is_ok());
        auto rs = std::move(sel.value());
        EXPECT_EQ(rs.row_count(), 1ULL);
        EXPECT_EQ(rs[0]["name"].as_text(), "mouse");
    }
}

static void test_api_transactions_and_rollback() {
    auto path = temp_test_file("test_public_api_txn");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    ScopedTempFile wal_cleanup(path.string() + ".wal");

    auto db = Database::open(path).value();
    EXPECT_TRUE(db->execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER);").is_ok());
    EXPECT_TRUE(db->execute("INSERT INTO accounts VALUES (1, 500);").is_ok());

    auto conn = db->connect().value();

    // 1. Explicit Rollback
    EXPECT_TRUE(conn->begin_transaction().is_ok());
    EXPECT_TRUE(conn->is_in_transaction());
    EXPECT_TRUE(conn->execute("UPDATE accounts SET balance = 1000 WHERE id = 1;").is_ok());

    // Verify modified in-transaction
    auto in_txn = conn->execute("SELECT balance FROM accounts WHERE id = 1;").value();
    EXPECT_EQ(in_txn[0][0].as_integer(), 1000);

    EXPECT_TRUE(conn->rollback().is_ok());
    EXPECT_FALSE(conn->is_in_transaction());

    // Verify restored post-rollback
    auto post_rb = conn->execute("SELECT balance FROM accounts WHERE id = 1;").value();
    EXPECT_EQ(post_rb[0][0].as_integer(), 500);

    // 2. TransactionGuard RAII automatic rollback on failure/exit
    {
        TransactionGuard guard(*conn);
        EXPECT_TRUE(guard.is_active());
        EXPECT_TRUE(conn->execute("UPDATE accounts SET balance = 9999 WHERE id = 1;").is_ok());
        // Do not call commit; let guard go out of scope
    }

    auto post_guard = conn->execute("SELECT balance FROM accounts WHERE id = 1;").value();
    EXPECT_EQ(post_guard[0][0].as_integer(), 500);

    // 3. TransactionGuard explicit commit
    {
        TransactionGuard guard(*conn);
        EXPECT_TRUE(conn->execute("UPDATE accounts SET balance = 750 WHERE id = 1;").is_ok());
        EXPECT_TRUE(guard.commit().is_ok());
        EXPECT_FALSE(guard.is_active());
    }

    auto post_commit = conn->execute("SELECT balance FROM accounts WHERE id = 1;").value();
    EXPECT_EQ(post_commit[0][0].as_integer(), 750);
}

int main() {
    std::cout << "=== Running Public C++ API Tests ===\n";

    run_test("test_api_version", test_api_version);
    run_test("test_api_database_lifecycle_and_crud", test_api_database_lifecycle_and_crud);
    run_test("test_api_transactions_and_rollback", test_api_transactions_and_rollback);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
