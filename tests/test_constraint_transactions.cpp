#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_explicit_transaction_rollback_on_constraint_failure() {
    auto db_path = temp_test_file("test_txn_rollback_db");
    ScopedTempFile cleanup_db(db_path);
    std::filesystem::path wal_path = db_path.string() + ".wal";

    auto pf = PageFile::create(db_path).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL);").is_ok());

    // 1. Begin explicit transaction
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());

    // Insert valid row 1
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, 'u1@example.com');").is_ok());

    // Insert row 2 with duplicate email - MUST FAIL
    auto res_viol = executor.execute("INSERT INTO users VALUES (2, 'u1@example.com');");
    EXPECT_TRUE(res_viol.is_err());
    EXPECT_EQ(res_viol.status().code(), StatusCode::ConstraintViolation);

    // Rollback explicit transaction
    EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());

    // Verify row 1 was rolled back and table is completely empty
    auto sel = executor.execute("SELECT * FROM users;");
    EXPECT_TRUE(sel.is_ok());
    EXPECT_EQ(sel.value().row_count(), 0ULL);
}

static void test_multi_row_update_atomicity() {
    auto db_path = temp_test_file("test_multi_update_db");
    ScopedTempFile cleanup_db(db_path);
    std::filesystem::path wal_path = db_path.string() + ".wal";

    auto pf = PageFile::create(db_path).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE scores (id INTEGER PRIMARY KEY, val INTEGER UNIQUE);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO scores VALUES (1, 100);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO scores VALUES (2, 200);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO scores VALUES (3, 300);").is_ok());

    // Attempt multi-row UPDATE where all rows would be set to 100 (violating UNIQUE)
    auto upd = executor.execute("UPDATE scores SET val = 100 WHERE id >= 1;");
    EXPECT_TRUE(upd.is_err());
    EXPECT_EQ(upd.status().code(), StatusCode::ConstraintViolation);

    // Verify no rows were partially modified
    auto sel1 = executor.execute("SELECT * FROM scores WHERE id = 1;");
    EXPECT_TRUE(sel1.is_ok());
    EXPECT_EQ(sel1.value().rows()[0].get(1).as_integer(), 100);

    auto sel2 = executor.execute("SELECT * FROM scores WHERE id = 2;");
    EXPECT_TRUE(sel2.is_ok());
    EXPECT_EQ(sel2.value().rows()[0].get(1).as_integer(), 200);

    auto sel3 = executor.execute("SELECT * FROM scores WHERE id = 3;");
    EXPECT_TRUE(sel3.is_ok());
    EXPECT_EQ(sel3.value().rows()[0].get(1).as_integer(), 300);
}

static void test_multi_row_delete_restrict_atomicity() {
    auto db_path = temp_test_file("test_multi_del_db");
    ScopedTempFile cleanup_db(db_path);
    std::filesystem::path wal_path = db_path.string() + ".wal";

    auto pf = PageFile::create(db_path).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE depts (id INTEGER PRIMARY KEY, name TEXT);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE TABLE emps (id INTEGER PRIMARY KEY, dept_id INTEGER REFERENCES depts(id));").is_ok());

    EXPECT_TRUE(executor.execute("INSERT INTO depts VALUES (1, 'Sales');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO depts VALUES (2, 'Finance');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO depts VALUES (3, 'HR');").is_ok());

    // Only department 2 is referenced by an employee
    EXPECT_TRUE(executor.execute("INSERT INTO emps VALUES (101, 2);").is_ok());

    // Multi-row DELETE on depts matching id >= 1
    // Department 1 has no children, but department 2 has a child.
    // RESTRICT on department 2 must abort the entire operation!
    auto del_res = executor.execute("DELETE FROM depts WHERE id >= 1;");
    EXPECT_TRUE(del_res.is_err());
    EXPECT_EQ(del_res.status().code(), StatusCode::ConstraintViolation);

    // Verify all 3 departments are still intact
    auto sel = executor.execute("SELECT * FROM depts;");
    EXPECT_TRUE(sel.is_ok());
    EXPECT_EQ(sel.value().row_count(), 3ULL);
}

int main() {
    std::cout << "Running Constraint Transactions & Atomicity Tests...\n";
    run_test("Explicit Transaction Rollback on Constraint Violation", test_explicit_transaction_rollback_on_constraint_failure);
    run_test("Multi-Row UPDATE Atomicity", test_multi_row_update_atomicity);
    run_test("Multi-Row DELETE RESTRICT Atomicity", test_multi_row_delete_restrict_atomicity);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
