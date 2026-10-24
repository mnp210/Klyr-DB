#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/slotted_page.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/record.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/index/key.hpp"
#include "klyrdb/index/bplus_tree.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::filesystem::path db_path = "demo_phase3.kdb";
    if (argc > 1) {
        db_path = argv[1];
    }

    std::cout << "======================================================================\n";
    std::cout << "    KlyrDB Phase 3: B+ Tree Indexing & Free-Space Map (FSM) Demo      \n";
    std::cout << "======================================================================\n\n";

    // Clean up any stale demo file
    std::error_code ec;
    if (std::filesystem::exists(db_path, ec)) {
        std::cout << "[Step 0] Cleaning up stale demo file: " << db_path.string() << "\n";
        std::filesystem::remove(db_path, ec);
    }

    // Step 1: Create database
    std::cout << "\n[Step 1] Creating database file: " << db_path.string() << "...\n";
    auto create_res = klyrdb::PageFile::create(db_path);
    if (create_res.is_err()) {
        std::cerr << "[-] Error creating database: " << create_res.status().to_string() << "\n";
        return 1;
    }
    std::cout << "[+] Database created successfully. Initial pages: "
              << create_res.value()->page_count() << " (Page 0 Metadata)\n";

    auto page_file = std::move(create_res.value());
    constexpr size_t POOL_SIZE = 16;

    // Step 2: Initialize BufferPoolManager, FreeSpaceMap, IndexCatalog, and RecordManager
    {
        std::cout << "\n[Step 2] Initializing Buffer Pool (" << POOL_SIZE << " frames), FSM, and Index Catalog...\n";
        klyrdb::BufferPoolManager bpm(*page_file, POOL_SIZE);

        auto fsm_res = klyrdb::FreeSpaceMap::open_or_create(bpm);
        if (fsm_res.is_err()) {
            std::cerr << "[-] Error initializing FSM: " << fsm_res.status().to_string() << "\n";
            return 1;
        }
        auto fsm = std::move(fsm_res.value());
        std::cout << "    [+] Persistent Free-Space Map loaded (Root Page: " << fsm->root_page_id() << ")\n";

        auto cat_res = klyrdb::IndexCatalog::open_or_create(bpm);
        if (cat_res.is_err()) {
            std::cerr << "[-] Error initializing IndexCatalog: " << cat_res.status().to_string() << "\n";
            return 1;
        }
        auto catalog = std::move(cat_res.value());
        std::cout << "    [+] Persistent Index Catalog loaded (Catalog Page: " << catalog->catalog_page_id() << ")\n";

        klyrdb::RecordManager rm(bpm, fsm.get());

        // Configure B+ Tree with low capacity to demonstrate node splitting
        klyrdb::BTreeConfig cfg;
        cfg.max_leaf_entries = 4;
        cfg.max_internal_entries = 4;
        klyrdb::BPlusTree tree(bpm, klyrdb::INVALID_PAGE_ID, cfg);

        tree.set_on_root_change([&catalog](klyrdb::page_id_t new_root) {
            auto meta = catalog->get_index("idx_employees_id");
            if (meta.is_ok()) {
                catalog->update_root_page("idx_employees_id", new_root);
            }
        });

        // Step 3: Insert sample records and index them
        std::cout << "\n[Step 3] Inserting employee records with FSM space tracking and B+ Tree indexing...\n";
        struct EmployeeRecord {
            int64_t id;
            std::string name;
            std::string department;
        };

        std::vector<EmployeeRecord> employees = {
            {101, "Alice Smith", "Engineering"},
            {102, "Bob Jones", "Product Management"},
            {103, "Charlie Brown", "Site Reliability Engineering"},
            {104, "Diana Prince", "Security Engineering"},
            {105, "Evan Wright", "Quality Assurance"},
            {106, "Fiona Gallagher", "Systems Operations"},
            {107, "George Clark", "Database Architecture"},
            {108, "Hannah Abbott", "Research & Development"}
        };

        // Insert first record to establish root
        std::string rec0 = employees[0].name + " | " + employees[0].department;
        auto rid0 = rm.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(rec0.data()), rec0.size())).value();
        tree.insert(klyrdb::IndexKey(employees[0].id), rid0);
        catalog->create_index("idx_employees_id", klyrdb::KeyType::Int64, tree.root_page_id());
        std::cout << "    [+] Registered index 'idx_employees_id' with Root Page " << tree.root_page_id() << "\n";

        for (size_t i = 1; i < employees.size(); ++i) {
            std::string rec = employees[i].name + " | " + employees[i].department;
            auto rid = rm.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(rec.data()), rec.size())).value();
            tree.insert(klyrdb::IndexKey(employees[i].id), rid);
            std::cout << "    [+] Indexed Employee #" << employees[i].id << " (" << employees[i].name
                      << ") -> " << rid.to_string() << "\n";
        }

        // Step 4: Perform Point Lookups via B+ Tree Index
        std::cout << "\n[Step 4] Executing Point Lookups via B+ Tree Index...\n";
        int64_t query_ids[] = {103, 107, 999};
        for (int64_t qid : query_ids) {
            auto lookup_res = tree.find_one(klyrdb::IndexKey(qid));
            if (lookup_res.is_ok()) {
                auto rid = lookup_res.value();
                auto raw_rec = rm.get_record(rid).value();
                std::string text(reinterpret_cast<const char*>(raw_rec.data()), raw_rec.size());
                std::cout << "    [+] ID " << qid << " FOUND at " << rid.to_string() << " => \"" << text << "\"\n";
            } else {
                std::cout << "    [-] ID " << qid << " NOT FOUND: " << lookup_res.status().message() << "\n";
            }
        }

        // Step 5: Execute Range Scan via B+ Tree Leaf Traversal
        std::cout << "\n[Step 5] Executing Range Scan [102 .. 106]...\n";
        auto range_res = tree.scan_range(klyrdb::IndexKey(int64_t{102}), klyrdb::IndexKey(int64_t{106}));
        if (range_res.is_ok()) {
            for (const auto& entry : range_res.value()) {
                auto data = rm.get_record(entry.rid).value();
                std::string text(reinterpret_cast<const char*>(data.data()), data.size());
                std::cout << "    [Scan] Key " << entry.key.as_int64() << " -> "
                          << entry.rid.to_string() << " => \"" << text << "\"\n";
            }
        }

        // Step 6: Free-Space Map Query
        std::cout << "\n[Step 6] Querying Free-Space Map for candidate page with >= 100 bytes...\n";
        auto candidate_page = fsm->find_page_with_space(100);
        if (candidate_page.is_ok()) {
            std::cout << "    [+] FSM candidate page found: Page " << candidate_page.value()
                      << " (Recorded free space: " << fsm->get_free_space(candidate_page.value()).value() << " bytes)\n";
        }

        // Flush all pages before closing
        std::cout << "\n[Step 7] Flushing buffer pool pages to disk...\n";
        bpm.flush_all_pages();
    }

    // Step 8: Reopen database file and verify persistence across restart
    std::cout << "\n[Step 8] Reopening database file from disk...\n";
    auto reopen_res = klyrdb::PageFile::open(db_path);
    if (reopen_res.is_err()) {
        std::cerr << "[-] Error reopening database: " << reopen_res.status().to_string() << "\n";
        return 1;
    }

    auto reopened_file = std::move(reopen_res.value());
    klyrdb::BufferPoolManager bpm(*reopened_file, POOL_SIZE);

    auto catalog = klyrdb::IndexCatalog::open_or_create(bpm).value();
    auto meta = catalog->get_index("idx_employees_id").value();
    std::cout << "    [+] Recovered index '" << meta.name << "' from Catalog (Root Page: " << meta.root_page_id << ")\n";

    klyrdb::BPlusTree restored_tree(bpm, meta.root_page_id);
    klyrdb::RecordManager rm(bpm);

    auto rid_104 = restored_tree.find_one(klyrdb::IndexKey(int64_t{104})).value();
    auto data_104 = rm.get_record(rid_104).value();
    std::string text_104(reinterpret_cast<const char*>(data_104.data()), data_104.size());
    std::cout << "    [+] Verified restored lookup for ID 104 => \"" << text_104 << "\"\n";

    std::cout << "\n======================================================================\n";
    std::cout << "             DEMO EXECUTION SUCCESSFULLY COMPLETED                    \n";
    std::cout << "======================================================================\n";

    return 0;
}
