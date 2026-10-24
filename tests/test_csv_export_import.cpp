#include "test_framework.hpp"
#include "klyrdb/backup/csv_migrator.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"
#include <fstream>
#include <string>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_csv_export_quoted_and_multiline() {
    auto db_path = temp_test_file("test_csv_quote_db");
    ScopedTempFile db_cleanup(db_path);
    auto csv_path = db_path.parent_path() / (db_path.filename().string() + ".csv");
    ScopedTempFile csv_cleanup(csv_path);

    {
        auto pf = PageFile::create(db_path, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE notes (id INTEGER PRIMARY KEY, content TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO notes VALUES (1, 'Simple text');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO notes VALUES (2, 'Comma, separated');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO notes VALUES (3, 'Quote \"test\"');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO notes VALUES (4, '');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO notes VALUES (5, NULL);").is_ok());

        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    CsvMigrator migrator;
    CsvExportOptions opts;
    auto exp_res = migrator.export_csv(db_path, "notes", csv_path, opts);
    EXPECT_TRUE(exp_res.is_ok());
    EXPECT_EQ(exp_res.value().rows_processed, 5ULL);

    // Verify CSV content
    std::ifstream ifs(csv_path);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ifs, line)) {
        lines.push_back(line);
    }
    EXPECT_TRUE(lines.size() >= 6ULL); // 1 header + 5 rows
    EXPECT_EQ(lines[0], "id,content");
}

static void test_csv_import_roundtrip() {
    auto db_path1 = temp_test_file("test_csv_rt_db1");
    ScopedTempFile db1_cleanup(db_path1);
    auto db_path2 = temp_test_file("test_csv_rt_db2");
    ScopedTempFile db2_cleanup(db_path2);
    auto csv_path = db_path1.parent_path() / (db_path1.filename().string() + ".csv");
    ScopedTempFile csv_cleanup(csv_path);

    // Setup source DB
    {
        auto pf = PageFile::create(db_path1, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE products (id INTEGER PRIMARY KEY, descr TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (10, 'A, B, C');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (20, '');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (30, NULL);").is_ok());
        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    CsvMigrator migrator;
    EXPECT_TRUE(migrator.export_csv(db_path1, "products", csv_path).is_ok());

    // Setup destination DB table
    {
        auto pf = PageFile::create(db_path2, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        EXPECT_TRUE(executor.execute("CREATE TABLE products (id INTEGER PRIMARY KEY, descr TEXT);").is_ok());
        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    // Import into destination
    EXPECT_TRUE(migrator.import_csv(csv_path, db_path2, "products").is_ok());

    // Verify imported records
    {
        auto pf = PageFile::open(db_path2, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto r1 = executor.execute("SELECT descr FROM products WHERE id = 10;").value();
        EXPECT_EQ(r1.rows()[0].get(0).as_text(), "A, B, C");

        auto r2 = executor.execute("SELECT descr FROM products WHERE id = 20;").value();
        EXPECT_FALSE(r2.rows()[0].get(0).is_null());
        EXPECT_EQ(r2.rows()[0].get(0).as_text(), "");

        auto r3 = executor.execute("SELECT descr FROM products WHERE id = 30;").value();
        EXPECT_TRUE(r3.rows()[0].get(0).is_null());
    }
}

static void test_csv_import_constraint_rejection() {
    auto db_path = temp_test_file("test_csv_cons_db");
    ScopedTempFile db_cleanup(db_path);
    auto csv_path = db_path.parent_path() / (db_path.filename().string() + ".csv");
    ScopedTempFile csv_cleanup(csv_path);

    {
        auto pf = PageFile::create(db_path, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        EXPECT_TRUE(executor.execute("CREATE TABLE staff (id INTEGER PRIMARY KEY, name TEXT NOT NULL);").is_ok());
        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    // Create CSV with NOT NULL constraint violation (row 2 has \N)
    {
        std::ofstream ofs(csv_path);
        ofs << "id,name\n";
        ofs << "1,Alice\n";
        ofs << "2,\\N\n";
    }

    CsvMigrator migrator;
    auto res = migrator.import_csv(csv_path, db_path, "staff");
    EXPECT_TRUE(res.is_err());
    EXPECT_EQ(res.status().code(), StatusCode::ConstraintViolation);
}

int main() {
    std::cout << "=== Running test_csv_export_import ===" << std::endl;
    run_test("test_csv_export_quoted_and_multiline", test_csv_export_quoted_and_multiline);
    run_test("test_csv_import_roundtrip", test_csv_import_roundtrip);
    run_test("test_csv_import_constraint_rejection", test_csv_import_constraint_rejection);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
