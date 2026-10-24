#include "test_framework.hpp"
#include "klyrdb/backup/logical_exporter.hpp"
#include "klyrdb/backup/logical_importer.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include <fstream>
#include <sstream>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_topological_export_import() {
    auto src_db = temp_test_file("test_exp_topo_src");
    ScopedTempFile src_cleanup(src_db);
    auto sql_dump = src_db.parent_path() / (src_db.filename().string() + ".sql");
    ScopedTempFile sql_cleanup(sql_dump);
    auto dest_db = temp_test_file("test_exp_topo_dest");
    ScopedTempFile dest_cleanup(dest_db);

    {
        auto pf = PageFile::create(src_db, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE depts (dept_id INTEGER PRIMARY KEY, name TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("CREATE TABLE emps (emp_id INTEGER PRIMARY KEY, name TEXT, dept_id INTEGER, FOREIGN KEY (dept_id) REFERENCES depts(dept_id));").is_ok());

        EXPECT_TRUE(executor.execute("INSERT INTO depts VALUES (10, 'Engineering');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO depts VALUES (20, 'Research');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO emps VALUES (1, 'Alice', 10);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO emps VALUES (2, 'Bob', 20);").is_ok());

        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    // 1. Export SQL
    LogicalExporter exporter;
    auto exp_res = exporter.export_sql(src_db, sql_dump);
    EXPECT_TRUE(exp_res.is_ok());
    EXPECT_EQ(exp_res.value().tables_exported, 2ULL);
    EXPECT_EQ(exp_res.value().rows_exported, 4ULL);

    // Verify ordering in SQL text: CREATE TABLE depts must appear before CREATE TABLE emps
    std::ifstream ifs(sql_dump);
    std::string sql_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    auto depts_pos = sql_content.find("CREATE TABLE depts");
    auto emps_pos = sql_content.find("CREATE TABLE emps");
    EXPECT_TRUE(depts_pos != std::string::npos);
    EXPECT_TRUE(emps_pos != std::string::npos);
    EXPECT_TRUE(depts_pos < emps_pos);

    // 2. Import SQL
    LogicalImporter importer;
    ImportOptions imp_opts;
    auto imp_res = importer.import_sql(sql_dump, dest_db, imp_opts);
    EXPECT_TRUE(imp_res.is_ok());
    EXPECT_EQ(imp_res.value().tables_imported, 2ULL);
    EXPECT_EQ(imp_res.value().rows_imported, 4ULL);

    // 3. Query imported DB
    {
        auto pf = PageFile::open(dest_db, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto q1 = executor.execute("SELECT * FROM depts WHERE dept_id = 10;");
        EXPECT_TRUE(q1.is_ok());
        EXPECT_EQ(q1.value().row_count(), 1ULL);
        EXPECT_EQ(q1.value().rows()[0].get(1).as_text(), "Engineering");

        auto q2 = executor.execute("SELECT * FROM emps WHERE emp_id = 1;");
        EXPECT_TRUE(q2.is_ok());
        EXPECT_EQ(q2.value().row_count(), 1ULL);
        EXPECT_EQ(q2.value().rows()[0].get(1).as_text(), "Alice");
    }
}

static void test_typed_literals_and_nulls() {
    auto src_db = temp_test_file("test_exp_null_src");
    ScopedTempFile src_cleanup(src_db);
    auto sql_dump = src_db.parent_path() / (src_db.filename().string() + ".sql");
    ScopedTempFile sql_cleanup(sql_dump);
    auto dest_db = temp_test_file("test_exp_null_dest");
    ScopedTempFile dest_cleanup(dest_db);

    {
        auto pf = PageFile::create(src_db, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE test_vals (id INTEGER PRIMARY KEY, note TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO test_vals VALUES (1, NULL);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO test_vals VALUES (2, '');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO test_vals VALUES (3, 'O''Reilly');").is_ok());

        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    LogicalExporter exporter;
    EXPECT_TRUE(exporter.export_sql(src_db, sql_dump).is_ok());

    LogicalImporter importer;
    EXPECT_TRUE(importer.import_sql(sql_dump, dest_db).is_ok());

    {
        auto pf = PageFile::open(dest_db, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto r1 = executor.execute("SELECT note FROM test_vals WHERE id = 1;");
        EXPECT_TRUE(r1.is_ok());
        EXPECT_TRUE(r1.value().rows()[0].get(0).is_null());

        auto r2 = executor.execute("SELECT note FROM test_vals WHERE id = 2;");
        EXPECT_TRUE(r2.is_ok());
        EXPECT_FALSE(r2.value().rows()[0].get(0).is_null());
        EXPECT_EQ(r2.value().rows()[0].get(0).as_text(), "");

        auto r3 = executor.execute("SELECT note FROM test_vals WHERE id = 3;");
        EXPECT_TRUE(r3.is_ok());
        EXPECT_EQ(r3.value().rows()[0].get(0).as_text(), "O'Reilly");
    }
}

static void test_transactional_rollback_on_bad_dump() {
    auto sql_dump = temp_test_file("test_exp_bad_dump");
    sql_dump = sql_dump.parent_path() / (sql_dump.filename().string() + ".sql");
    ScopedTempFile sql_cleanup(sql_dump);
    auto dest_db = temp_test_file("test_exp_bad_dest");
    ScopedTempFile dest_cleanup(dest_db);

    // Create an invalid SQL dump file
    {
        std::ofstream ofs(sql_dump);
        ofs << "BEGIN TRANSACTION;\n";
        ofs << "CREATE TABLE valid_table (id INTEGER PRIMARY KEY);\n";
        ofs << "INSERT INTO valid_table VALUES (1);\n";
        ofs << "THIS IS NOT VALID SQL SYNTAX AT ALL;\n";
        ofs << "COMMIT;\n";
    }

    LogicalImporter importer;
    ImportOptions opts;
    opts.stop_on_error = true;
    auto res = importer.import_sql(sql_dump, dest_db, opts);
    EXPECT_TRUE(res.is_err());

    // Because staging disposal was applied, target database is clean or discarded
    if (std::filesystem::exists(dest_db)) {
        auto pf = PageFile::open(dest_db, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto tc = TableCatalog::open_or_create(bpm).value();
        EXPECT_TRUE(tc->list_tables().empty());
    }
}

int main() {
    std::cout << "=== Running test_logical_export_import ===" << std::endl;
    run_test("test_topological_export_import", test_topological_export_import);
    run_test("test_typed_literals_and_nulls", test_typed_literals_and_nulls);
    run_test("test_transactional_rollback_on_bad_dump", test_transactional_rollback_on_bad_dump);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
