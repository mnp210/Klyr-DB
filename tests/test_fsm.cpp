#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/storage/record_manager.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_fsm_init() {
    auto path = temp_test_file("test_fsm_init");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);

    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    EXPECT_NE(fsm->root_page_id(), INVALID_PAGE_ID);

    // Initial FSM has no candidate pages for large allocations
    auto find_res = fsm->find_page_with_space(500);
    EXPECT_TRUE(find_res.is_err());
    EXPECT_EQ(find_res.status().code(), StatusCode::NoFreeSpace);
}

static void test_fsm_updates_and_search() {
    auto path = temp_test_file("test_fsm_search");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();

    // Record free space for hypothetical data pages 2, 3, 4 (page 1 is FSM root)
    EXPECT_TRUE(fsm->update_free_space(2, 100).is_ok());
    EXPECT_TRUE(fsm->update_free_space(3, 500).is_ok());
    EXPECT_TRUE(fsm->update_free_space(4, 1200).is_ok());

    EXPECT_EQ(fsm->get_free_space(2).value(), 100ULL);
    EXPECT_EQ(fsm->get_free_space(3).value(), 500ULL);
    EXPECT_EQ(fsm->get_free_space(4).value(), 1200ULL);

    // Search for 50 bytes -> page 2 qualifies
    EXPECT_EQ(fsm->find_page_with_space(50).value(), 2U);

    // Search for 300 bytes -> page 3 qualifies
    EXPECT_EQ(fsm->find_page_with_space(300).value(), 3U);

    // Search for 1000 bytes -> page 4 qualifies
    EXPECT_EQ(fsm->find_page_with_space(1000).value(), 4U);

    // Search for 2000 bytes -> none qualify
    auto no_match = fsm->find_page_with_space(2000);
    EXPECT_TRUE(no_match.is_err());
    EXPECT_EQ(no_match.status().code(), StatusCode::NoFreeSpace);

    // Decrease free space of page 3 after record inserted
    EXPECT_TRUE(fsm->update_free_space(3, 50).is_ok());
    EXPECT_EQ(fsm->get_free_space(3).value(), 50ULL);
    EXPECT_EQ(fsm->find_page_with_space(300).value(), 4U);
}

static void test_fsm_rebuild() {
    auto path = temp_test_file("test_fsm_rebuild");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);

    // Insert records via RecordManager without FSM
    RecordManager rm(bpm);
    std::vector<uint8_t> payload_200(200, 0xAB);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rm.insert_record(payload_200).is_ok());
    }

    // Now initialize FSM and rebuild
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    EXPECT_TRUE(fsm->rebuild().is_ok());

    // Data page 1 should have recorded free space > 0
    size_t fsp_page1 = fsm->get_free_space(1).value();
    EXPECT_TRUE(fsp_page1 > 0);
    EXPECT_TRUE(fsp_page1 < 4000);
}

static void test_fsm_persistence() {
    auto path = temp_test_file("test_fsm_persist");
    ScopedTempFile cleanup(path);

    page_id_t fsm_root = INVALID_PAGE_ID;

    // Phase 1: Create FSM and update
    {
        auto pf = PageFile::create(path).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        fsm_root = fsm->root_page_id();

        EXPECT_TRUE(fsm->update_free_space(5, 1500).is_ok());
        EXPECT_TRUE(fsm->update_free_space(8, 2500).is_ok());
        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    }

    // Phase 2: Reopen from disk
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();

        EXPECT_EQ(fsm->root_page_id(), fsm_root);
        EXPECT_EQ(fsm->get_free_space(5).value(), 1500ULL);
        EXPECT_EQ(fsm->get_free_space(8).value(), 2500ULL);
        EXPECT_EQ(fsm->find_page_with_space(2000).value(), 8U);
    }
}

int main() {
    std::cout << "=== Running KlyrDB Free-Space Map (FSM) Tests ===\n";

    run_test("FSMInit", test_fsm_init);
    run_test("FSMUpdatesAndSearch", test_fsm_updates_and_search);
    run_test("FSMRebuild", test_fsm_rebuild);
    run_test("FSMPersistence", test_fsm_persistence);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
