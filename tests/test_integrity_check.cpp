#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/storage/integrity_checker.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_healthy_database_integrity() {
    auto path = temp_test_file("test_integ_healthy");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE depts (dept_id INTEGER PRIMARY KEY, name TEXT NOT NULL);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO depts VALUES (10, 'Engineering');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO depts VALUES (20, 'Sales');").is_ok());

    EXPECT_TRUE(executor.execute("CREATE TABLE emps (emp_id INTEGER PRIMARY KEY, name TEXT NOT NULL, dept_id INTEGER REFERENCES depts(dept_id));").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_emp_name ON emps(name);").is_ok());

    EXPECT_TRUE(executor.execute("INSERT INTO emps VALUES (1, 'Alice', 10);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO emps VALUES (2, 'Bob', 20);").is_ok());

    EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    EXPECT_TRUE(pf->sync().is_ok());

    // Run integrity check
    IntegrityChecker checker(*pf, wal.get());
    auto rep_res = checker.check_all();
    EXPECT_TRUE(rep_res.is_ok());

    const auto& report = rep_res.value();
    EXPECT_TRUE(report.is_healthy);
    EXPECT_EQ(report.error_count(), 0ULL);
    EXPECT_EQ(report.tables_checked, 2ULL);
    EXPECT_EQ(report.records_checked, 4ULL);
    EXPECT_TRUE(report.indexes_checked >= 1ULL);
}

static void test_foreign_key_integrity_violation() {
    auto path = temp_test_file("test_integ_fk_violation");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE parent (id INTEGER PRIMARY KEY);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO parent VALUES (1);").is_ok());

    EXPECT_TRUE(executor.execute("CREATE TABLE child (cid INTEGER PRIMARY KEY, pid INTEGER REFERENCES parent(id));").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO child VALUES (100, 1);").is_ok());

    // Inject an orphaned child row directly via RecordManager (bypassing SQL parser validation)
    // to simulate relational constraint corruption on disk
    auto child_tbl = tc->get_table("child").value();
    SqlRow orphan_row({SqlValue(int64_t(200)), SqlValue(int64_t(999))});
    auto orphan_bytes = orphan_row.encode(child_tbl.table_id);
    EXPECT_TRUE(rm.insert_record(orphan_bytes).is_ok());

    EXPECT_TRUE(bpm.flush_all_pages().is_ok());

    IntegrityChecker checker(*pf, nullptr);
    auto rep_res = checker.check_all();
    EXPECT_TRUE(rep_res.is_ok());

    const auto& report = rep_res.value();
    EXPECT_FALSE(report.is_healthy);
    EXPECT_TRUE(report.error_count() >= 1);

    bool found_fk_error = false;
    for (const auto& issue : report.issues) {
        if (issue.subsystem == "Constraint" && issue.message.find("Foreign key violation") != std::string::npos) {
            found_fk_error = true;
            break;
        }
    }
    EXPECT_TRUE(found_fk_error);
}

int main() {
    std::cout << "=== Running Storage Integrity Tests ===\n";
    run_test("test_healthy_database_integrity", test_healthy_database_integrity);
    run_test("test_foreign_key_integrity_violation", test_foreign_key_integrity_violation);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
