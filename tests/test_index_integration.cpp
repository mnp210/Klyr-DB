#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/index/key.hpp"
#include "klyrdb/index/bplus_tree.hpp"
#include "klyrdb/index/index_catalog.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_fsm_assisted_insertions() {
    auto path = temp_test_file("test_fsm_integration");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);

    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());

    // Insert 50 records of 100 bytes each
    std::vector<RecordId> rids;
    for (int i = 0; i < 50; ++i) {
        std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
        auto rid_res = rm.insert_record(data);
        EXPECT_TRUE(rid_res.is_ok());
        rids.push_back(rid_res.value());
    }

    // Verify all 50 records retrievable
    for (size_t i = 0; i < rids.size(); ++i) {
        auto rec = rm.get_record(rids[i]).value();
        EXPECT_EQ(rec.size(), 100ULL);
        EXPECT_EQ(rec[0], static_cast<uint8_t>(i));
    }

    // Free space map should accurately track pages
    for (const auto& rid : rids) {
        size_t fsp = fsm->get_free_space(rid.page_id).value();
        EXPECT_TRUE(fsp > 0);
        EXPECT_TRUE(fsp < 4000);
    }
}

static void test_end_to_end_indexed_records() {
    auto path = temp_test_file("test_end_to_end_idx");
    ScopedTempFile cleanup(path);

    // Phase 1: Insert records and build indexes
    {
        auto pf = PageFile::create(path).value();
        BufferPoolManager bpm(*pf, 32);

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        auto catalog = IndexCatalog::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());

        BTreeConfig cfg;
        cfg.max_leaf_entries = 8;
        cfg.max_internal_entries = 8;

        BPlusTree pk_tree(bpm, INVALID_PAGE_ID, cfg);
        pk_tree.set_on_root_change([&catalog](page_id_t new_root) {
            catalog->update_root_page("idx_emp_id", new_root);
        });

        // Insert first record and initialize index
        std::string name_1 = "Alice";
        auto rid_1 = rm.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(name_1.data()), name_1.size())).value();
        EXPECT_TRUE(pk_tree.insert(IndexKey(101), rid_1).is_ok());
        EXPECT_TRUE(catalog->create_index("idx_emp_id", KeyType::Int64, pk_tree.root_page_id()).is_ok());

        // Insert employees with IDs 102 .. 150
        for (int64_t emp_id = 102; emp_id <= 150; ++emp_id) {
            std::string name = "Emp_" + std::to_string(emp_id);
            auto rid = rm.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(name.data()), name.size())).value();
            EXPECT_TRUE(pk_tree.insert(IndexKey(emp_id), rid).is_ok());
        }

        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    }

    // Phase 2: Reopen and query via index
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 32);

        auto catalog = IndexCatalog::open_or_create(bpm).value();
        auto meta = catalog->get_index("idx_emp_id").value();

        BTreeConfig cfg;
        cfg.max_leaf_entries = 8;
        cfg.max_internal_entries = 8;
        BPlusTree pk_tree(bpm, meta.root_page_id, cfg);
        RecordManager rm(bpm);

        // Point lookup for emp_id 125
        auto rid_125 = pk_tree.find_one(IndexKey(125)).value();
        auto data_125 = rm.get_record(rid_125).value();
        std::string name_125(reinterpret_cast<const char*>(data_125.data()), data_125.size());
        EXPECT_EQ(name_125, "Emp_125");

        // Range scan for IDs [110, 115]
        auto range = pk_tree.scan_range(IndexKey(110), IndexKey(115), true, true).value();
        EXPECT_EQ(range.size(), 6ULL);
        for (size_t i = 0; i < range.size(); ++i) {
            auto rid = range[i].rid;
            auto data = rm.get_record(rid).value();
            std::string expected = "Emp_" + std::to_string(110 + i);
            std::string actual(reinterpret_cast<const char*>(data.data()), data.size());
            EXPECT_EQ(actual, expected);
        }
    }
}

static void test_buffer_pool_pressure_with_tree() {
    auto path = temp_test_file("test_bpm_pressure_tree");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    // Tiny buffer pool of only 6 frames to force frequent eviction and page re-fetches
    BufferPoolManager bpm(*pf, 6);

    BTreeConfig cfg;
    cfg.max_leaf_entries = 4;
    cfg.max_internal_entries = 4;
    BPlusTree tree(bpm, INVALID_PAGE_ID, cfg);

    // Insert 60 entries (will allocate ~20 leaf and internal pages, far exceeding 6 frames)
    for (int64_t i = 1; i <= 60; ++i) {
        EXPECT_TRUE(tree.insert(IndexKey(i * 3), RecordId{static_cast<page_id_t>(i), 0}).is_ok());
    }

    // Lookups must trigger transparent disk reads and cache replacements
    for (int64_t i = 1; i <= 60; ++i) {
        auto rid = tree.find_one(IndexKey(i * 3)).value();
        EXPECT_EQ(rid.page_id, static_cast<page_id_t>(i));
    }

    // Reverse lookups
    for (int64_t i = 60; i >= 1; --i) {
        auto rid = tree.find_one(IndexKey(i * 3)).value();
        EXPECT_EQ(rid.page_id, static_cast<page_id_t>(i));
    }
}

int main() {
    std::cout << "=== Running KlyrDB Index Integration Tests ===\n";

    run_test("FSMAssistedInsertions", test_fsm_assisted_insertions);
    run_test("EndToEndIndexedRecords", test_end_to_end_indexed_records);
    run_test("BufferPoolPressureWithTree", test_buffer_pool_pressure_with_tree);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
