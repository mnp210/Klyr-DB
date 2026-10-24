#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace klyrdb;
using namespace klyrdb::testing;
using namespace std::chrono_literals;

static void test_concurrent_select_queries() {
    auto path = temp_test_file("test_concurrent_reads");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    auto pf = PageFile::create(path, false).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);
    EXPECT_TRUE(executor.execute("CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT, price INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_price ON products(price);").is_ok());

    // Insert 100 records
    constexpr int total_records = 100;
    for (int i = 1; i <= total_records; ++i) {
        std::string ins = "INSERT INTO products VALUES (" + std::to_string(i) + ", 'product_" +
                          std::to_string(i) + "', " + std::to_string(i * 10) + ");";
        EXPECT_TRUE(executor.execute(ins).is_ok());
    }

    // Launch concurrent reader threads
    constexpr size_t num_readers = 6;
    constexpr size_t iterations = 30;
    std::atomic<size_t> successful_reads{0};
    std::atomic<size_t> failed_reads{0};
    std::vector<std::thread> readers;

    for (size_t t = 0; t < num_readers; ++t) {
        readers.emplace_back([&, t]() {
            for (size_t iter = 0; iter < iterations; ++iter) {
                // Alternate query types based on thread ID
                if (t % 3 == 0) {
                    // Point lookup on primary key
                    int target_id = static_cast<int>((iter % total_records) + 1);
                    std::string q = "SELECT id, name, price FROM products WHERE id = " + std::to_string(target_id) + ";";
                    auto res = executor.execute(q);
                    if (res.is_ok() && res.value().row_count() == 1) {
                        EXPECT_EQ(res.value().rows()[0].get(0).as_integer(), static_cast<int64_t>(target_id));
                        successful_reads.fetch_add(1);
                    } else {
                        failed_reads.fetch_add(1);
                    }
                } else if (t % 3 == 1) {
                    // Secondary index range scan
                    std::string q = "SELECT id, price FROM products WHERE price >= 200 AND price <= 400;";
                    auto res = executor.execute(q);
                    if (res.is_ok() && res.value().row_count() == 21) {
                        successful_reads.fetch_add(1);
                    } else {
                        failed_reads.fetch_add(1);
                    }
                } else {
                    // Full table scan / count
                    std::string q = "SELECT id FROM products;";
                    auto res = executor.execute(q);
                    if (res.is_ok() && res.value().row_count() == total_records) {
                        successful_reads.fetch_add(1);
                    } else {
                        failed_reads.fetch_add(1);
                    }
                }
            }
        });
    }

    for (auto& r : readers) {
        r.join();
    }

    EXPECT_EQ(failed_reads.load(), 0ULL);
    EXPECT_EQ(successful_reads.load(), num_readers * iterations);
}

int main() {
    std::cout << "=== Running Concurrent Reads Tests ===\n";
    run_test("test_concurrent_select_queries", test_concurrent_select_queries);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
