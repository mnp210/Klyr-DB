#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/index/key.hpp"
#include "klyrdb/index/bplus_tree.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include <numeric>
#include <random>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_empty_tree() {
    auto path = temp_test_file("test_btree_empty");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);
    BPlusTree tree(bpm);

    EXPECT_TRUE(tree.is_empty());
    EXPECT_EQ(tree.root_page_id(), INVALID_PAGE_ID);

    auto res = tree.find_one(IndexKey(42));
    EXPECT_TRUE(res.is_err());
    EXPECT_EQ(res.status().code(), StatusCode::KeyNotFound);

    auto find_all = tree.find(IndexKey(42));
    EXPECT_TRUE(find_all.is_err());

    auto range = tree.scan_range(IndexKey(10), IndexKey(50));
    EXPECT_TRUE(range.is_ok());
    EXPECT_EQ(range.value().size(), 0ULL);

    auto it = tree.begin();
    EXPECT_FALSE(it.is_valid());
    EXPECT_TRUE(it == tree.end());
}

static void test_single_leaf_ops() {
    auto path = temp_test_file("test_btree_single_leaf");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);
    BPlusTree tree(bpm);

    // Insert 10 ordered entries
    for (int64_t i = 1; i <= 10; ++i) {
        Status s = tree.insert(IndexKey(i * 10), RecordId{1, static_cast<slot_id_t>(i)});
        EXPECT_TRUE(s.is_ok());
    }

    EXPECT_FALSE(tree.is_empty());

    // Point lookups
    for (int64_t i = 1; i <= 10; ++i) {
        auto res = tree.find_one(IndexKey(i * 10));
        EXPECT_TRUE(res.is_ok());
        EXPECT_EQ(res.value().page_id, 1U);
        EXPECT_EQ(res.value().slot_id, static_cast<slot_id_t>(i));
    }

    // Missing key
    auto missing = tree.find_one(IndexKey(25));
    EXPECT_TRUE(missing.is_err());
    EXPECT_EQ(missing.status().code(), StatusCode::KeyNotFound);

    // Duplicate rejection for identical (key, rid)
    Status dup = tree.insert(IndexKey(50), RecordId{1, 5});
    EXPECT_TRUE(dup.is_err());
    EXPECT_EQ(dup.code(), StatusCode::DuplicateKey);

    // Range scan [30, 70]
    auto range = tree.scan_range(IndexKey(30), IndexKey(70), true, true).value();
    EXPECT_EQ(range.size(), 5ULL);
    EXPECT_EQ(range[0].key.as_int64(), 30);
    EXPECT_EQ(range[4].key.as_int64(), 70);

    // Delete middle entry
    Status del = tree.delete_entry(IndexKey(50), RecordId{1, 5});
    EXPECT_TRUE(del.is_ok());
    EXPECT_TRUE(tree.find_one(IndexKey(50)).is_err());
}

static void test_duplicate_logical_keys() {
    auto path = temp_test_file("test_btree_duplicates");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);
    BPlusTree tree(bpm);

    // Insert multiple records with the same logical key 100
    EXPECT_TRUE(tree.insert(IndexKey(100), RecordId{1, 3}).is_ok());
    EXPECT_TRUE(tree.insert(IndexKey(100), RecordId{1, 1}).is_ok());
    EXPECT_TRUE(tree.insert(IndexKey(100), RecordId{2, 0}).is_ok());
    EXPECT_TRUE(tree.insert(IndexKey(50),  RecordId{1, 2}).is_ok());
    EXPECT_TRUE(tree.insert(IndexKey(200), RecordId{2, 1}).is_ok());

    auto all_100 = tree.find(IndexKey(100)).value();
    EXPECT_EQ(all_100.size(), 3ULL);
    // Should be deterministically sorted by RecordId
    EXPECT_EQ(all_100[0].page_id, 1U);
    EXPECT_EQ(all_100[0].slot_id, 1U);
    EXPECT_EQ(all_100[1].page_id, 1U);
    EXPECT_EQ(all_100[1].slot_id, 3U);
    EXPECT_EQ(all_100[2].page_id, 2U);
    EXPECT_EQ(all_100[2].slot_id, 0U);

    // Delete one duplicate
    EXPECT_TRUE(tree.delete_entry(IndexKey(100), RecordId{1, 3}).is_ok());
    auto remaining_100 = tree.find(IndexKey(100)).value();
    EXPECT_EQ(remaining_100.size(), 2ULL);
    EXPECT_EQ(remaining_100[0], (RecordId{1, 1}));
    EXPECT_EQ(remaining_100[1], (RecordId{2, 0}));
}

static void test_string_keys() {
    auto path = temp_test_file("test_btree_strings");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);
    BPlusTree tree(bpm);

    std::vector<std::string> names = {"alice", "bob", "charlie", "david", "eve", "frank", "grace"};
    for (size_t i = 0; i < names.size(); ++i) {
        EXPECT_TRUE(tree.insert(IndexKey(names[i]), RecordId{1, static_cast<slot_id_t>(i)}).is_ok());
    }

    // Lookup
    for (size_t i = 0; i < names.size(); ++i) {
        auto rid = tree.find_one(IndexKey(names[i])).value();
        EXPECT_EQ(rid.slot_id, static_cast<slot_id_t>(i));
    }

    // Range scan ["bob", "eve"]
    auto range = tree.scan_range(IndexKey("bob"), IndexKey("eve"), true, true).value();
    EXPECT_EQ(range.size(), 4ULL);
    EXPECT_EQ(range[0].key.as_string(), "bob");
    EXPECT_EQ(range[1].key.as_string(), "charlie");
    EXPECT_EQ(range[2].key.as_string(), "david");
    EXPECT_EQ(range[3].key.as_string(), "eve");
}

static void test_leaf_splitting_and_links() {
    auto path = temp_test_file("test_btree_split");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);

    // Configure small leaf capacity to force split quickly
    BTreeConfig cfg;
    cfg.max_leaf_entries = 4;
    cfg.max_internal_entries = 4;
    BPlusTree tree(bpm, INVALID_PAGE_ID, cfg);

    for (int64_t i = 1; i <= 8; ++i) {
        EXPECT_TRUE(tree.insert(IndexKey(i * 10), RecordId{1, static_cast<slot_id_t>(i)}).is_ok());
    }

    // All 8 keys must be retrievable
    for (int64_t i = 1; i <= 8; ++i) {
        auto rid = tree.find_one(IndexKey(i * 10));
        EXPECT_TRUE(rid.is_ok());
        EXPECT_EQ(rid.value().slot_id, static_cast<slot_id_t>(i));
    }

    // Range scan over all entries through sibling leaf links
    auto all = tree.scan_range().value();
    EXPECT_EQ(all.size(), 8ULL);
    for (size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(all[i].key.as_int64(), static_cast<int64_t>((i + 1) * 10));
    }
}

static void test_multilevel_tree_splits() {
    auto path = temp_test_file("test_btree_multilevel");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 64);

    BTreeConfig cfg;
    cfg.max_leaf_entries = 4;
    cfg.max_internal_entries = 4;
    BPlusTree tree(bpm, INVALID_PAGE_ID, cfg);

    constexpr int NUM_KEYS = 100;
    std::vector<int64_t> keys(NUM_KEYS);
    std::iota(keys.begin(), keys.end(), 1);

    // Shuffle insertion order to test random tree growth
    std::mt19937 g(12345);
    std::shuffle(keys.begin(), keys.end(), g);

    for (int64_t k : keys) {
        Status s = tree.insert(IndexKey(k), RecordId{static_cast<page_id_t>(k / 10 + 1), static_cast<slot_id_t>(k % 10)});
        EXPECT_TRUE(s.is_ok());
    }

    // Verify all keys present
    for (int64_t k = 1; k <= NUM_KEYS; ++k) {
        auto res = tree.find_one(IndexKey(k));
        EXPECT_TRUE(res.is_ok());
        EXPECT_EQ(res.value().page_id, static_cast<page_id_t>(k / 10 + 1));
        EXPECT_EQ(res.value().slot_id, static_cast<slot_id_t>(k % 10));
    }

    // Range scan should yield perfectly sorted 1..100
    auto all = tree.scan_range().value();
    EXPECT_EQ(all.size(), static_cast<size_t>(NUM_KEYS));
    for (int64_t i = 0; i < NUM_KEYS; ++i) {
        EXPECT_EQ(all[static_cast<size_t>(i)].key.as_int64(), i + 1);
    }
}

static void test_deletions_and_underflow() {
    auto path = temp_test_file("test_btree_delete");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 64);

    BTreeConfig cfg;
    cfg.max_leaf_entries = 4;
    cfg.max_internal_entries = 4;
    BPlusTree tree(bpm, INVALID_PAGE_ID, cfg);

    for (int64_t i = 1; i <= 20; ++i) {
        EXPECT_TRUE(tree.insert(IndexKey(i), RecordId{1, static_cast<slot_id_t>(i)}).is_ok());
    }

    // Delete odd keys
    for (int64_t i = 1; i <= 19; i += 2) {
        Status del = tree.delete_entry(IndexKey(i), RecordId{1, static_cast<slot_id_t>(i)});
        EXPECT_TRUE(del.is_ok());
    }

    // Verify odd keys gone, even keys remain
    for (int64_t i = 1; i <= 20; ++i) {
        auto res = tree.find_one(IndexKey(i));
        if (i % 2 == 1) {
            EXPECT_TRUE(res.is_err());
        } else {
            EXPECT_TRUE(res.is_ok());
            EXPECT_EQ(res.value().slot_id, static_cast<slot_id_t>(i));
        }
    }

    // Verify range scan only returns even keys
    auto evens = tree.scan_range().value();
    EXPECT_EQ(evens.size(), 10ULL);
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(evens[i].key.as_int64(), static_cast<int64_t>((i + 1) * 2));
    }
}

static void test_iterator_traversal() {
    auto path = temp_test_file("test_btree_iterator");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);

    BTreeConfig cfg;
    cfg.max_leaf_entries = 4;
    cfg.max_internal_entries = 4;
    BPlusTree tree(bpm, INVALID_PAGE_ID, cfg);

    for (int64_t i = 1; i <= 25; ++i) {
        EXPECT_TRUE(tree.insert(IndexKey(i * 2), RecordId{1, static_cast<slot_id_t>(i)}).is_ok());
    }

    // Iterate through full tree
    int64_t expected = 2;
    size_t count = 0;
    for (auto it = tree.begin(); it != tree.end(); ++it) {
        EXPECT_EQ(it->key.as_int64(), expected);
        expected += 2;
        count++;
    }
    EXPECT_EQ(count, 25ULL);

    // Lower bound test for key 21 (first key >= 21 is 22)
    auto it_lb = tree.lower_bound(IndexKey(21));
    EXPECT_TRUE(it_lb.is_valid());
    EXPECT_EQ(it_lb->key.as_int64(), 22);
}

static void test_catalog_and_persistence() {
    auto path = temp_test_file("test_btree_persist");
    ScopedTempFile cleanup(path);

    // Phase A: Create database, register index in catalog, insert entries
    {
        auto pf = PageFile::create(path).value();
        BufferPoolManager bpm(*pf, 16);

        auto catalog = IndexCatalog::open_or_create(bpm).value();
        EXPECT_TRUE(catalog->list_indexes().value().empty());

        BPlusTree tree(bpm);
        tree.set_on_root_change([&catalog](page_id_t new_root) {
            auto meta = catalog->get_index("idx_users_id");
            if (meta.is_ok()) {
                catalog->update_root_page("idx_users_id", new_root);
            }
        });

        // Insert first entry to create root
        EXPECT_TRUE(tree.insert(IndexKey(500), RecordId{1, 1}).is_ok());
        EXPECT_TRUE(catalog->create_index("idx_users_id", KeyType::Int64, tree.root_page_id()).is_ok());

        // Insert 30 more entries (10 .. 300)
        for (int64_t i = 1; i <= 30; ++i) {
            EXPECT_TRUE(tree.insert(IndexKey(i * 10), RecordId{2, static_cast<slot_id_t>(i)}).is_ok());
        }

        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    }

    // Phase B: Reopen database from disk, load catalog, resume tree operations
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 16);

        auto catalog = IndexCatalog::open_or_create(bpm).value();
        auto idx_meta = catalog->get_index("idx_users_id").value();
        EXPECT_EQ(idx_meta.name, "idx_users_id");
        EXPECT_NE(idx_meta.root_page_id, INVALID_PAGE_ID);

        BPlusTree tree(bpm, idx_meta.root_page_id);
        EXPECT_FALSE(tree.is_empty());

        // Verify initial key and all 30 inserted keys
        EXPECT_EQ(tree.find_one(IndexKey(500)).value(), (RecordId{1, 1}));
        for (int64_t i = 1; i <= 30; ++i) {
            auto rid = tree.find_one(IndexKey(i * 10)).value();
            EXPECT_EQ(rid.page_id, 2U);
            EXPECT_EQ(rid.slot_id, static_cast<slot_id_t>(i));
        }
    }
}

int main() {
    std::cout << "=== Running KlyrDB B+ Tree Index Tests ===\n";

    run_test("EmptyTree", test_empty_tree);
    run_test("SingleLeafOps", test_single_leaf_ops);
    run_test("DuplicateLogicalKeys", test_duplicate_logical_keys);
    run_test("StringKeys", test_string_keys);
    run_test("LeafSplittingAndLinks", test_leaf_splitting_and_links);
    run_test("MultilevelTreeSplits", test_multilevel_tree_splits);
    run_test("DeletionsAndUnderflow", test_deletions_and_underflow);
    run_test("IteratorTraversal", test_iterator_traversal);
    run_test("CatalogAndPersistence", test_catalog_and_persistence);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
