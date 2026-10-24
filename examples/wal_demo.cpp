#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/sql/sql_shell.hpp"
#include <iostream>
#include <filesystem>
#include <string>

int main(int argc, char* argv[]) {
    std::filesystem::path db_path = "demo_phase5.kdb";
    if (argc > 1) {
        db_path = argv[1];
    }
    std::filesystem::path wal_path = db_path.string() + ".wal";

    std::cout << "======================================================================\n";
    std::cout << "  KlyrDB Phase 5: Write-Ahead Logging, Transactions & Crash Recovery \n";
    std::cout << "======================================================================\n\n";

    // Clean up previous demo files
    std::error_code ec;
    if (std::filesystem::exists(db_path, ec)) std::filesystem::remove(db_path, ec);
    if (std::filesystem::exists(wal_path, ec)) std::filesystem::remove(wal_path, ec);

    // Scope 1: Initial database, DDL, baseline, rollback demo, and committed crash demo
    {
        std::cout << "[Step 1] Creating database file: " << db_path.string() << " and WAL...\n";
        auto pf_res = klyrdb::PageFile::create(db_path);
        if (pf_res.is_err()) {
            std::cerr << "[-] Error creating database: " << pf_res.status().to_string() << "\n";
            return 1;
        }
        auto pf = std::move(pf_res.value());

        auto wal_res = klyrdb::WriteAheadLog::open_or_create(wal_path);
        if (wal_res.is_err()) {
            std::cerr << "[-] Error creating WAL: " << wal_res.status().to_string() << "\n";
            return 1;
        }
        auto wal = std::move(wal_res.value());

        klyrdb::BufferPoolManager bpm(*pf, 32);
        bpm.set_wal(wal.get());

        klyrdb::TransactionManager tm(bpm, *wal);
        auto fsm = klyrdb::FreeSpaceMap::open_or_create(bpm).value();
        klyrdb::RecordManager rm(bpm, fsm.get());
        auto ic = klyrdb::IndexCatalog::open_or_create(bpm).value();
        auto tc = klyrdb::TableCatalog::open_or_create(bpm).value();

        klyrdb::SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        std::cout << "[+] Database and WAL initialized. WAL size: " << wal->file_size() << " bytes.\n\n";

        // Step 2: DDL
        std::cout << "[Step 2] Executing DDL: CREATE TABLE bank_accounts...\n";
        (void)executor.execute("CREATE TABLE bank_accounts (id INTEGER, owner TEXT, balance INTEGER);");
        (void)executor.execute("CREATE INDEX idx_acc_id ON bank_accounts (id);");
        (void)bpm.flush_all_pages();
        std::cout << "[+] Table 'bank_accounts' and index 'idx_acc_id' registered.\n\n";

        // Step 3: Populate baseline
        std::cout << "[Step 3] Inserting baseline accounts...\n";
        (void)executor.execute("INSERT INTO bank_accounts VALUES (1, 'Alice', 1000);");
        (void)executor.execute("INSERT INTO bank_accounts VALUES (2, 'Bob', 500);");
        (void)bpm.flush_all_pages();
        std::cout << "[+] Baseline inserted and durable.\n\n";

        // Step 4: Transaction Rollback Demo
        std::cout << "[Step 4] Demonstration: Multi-page Transaction Rollback...\n";
        std::cout << "  > BEGIN;\n";
        (void)executor.execute("BEGIN;");
        std::cout << "  > UPDATE bank_accounts SET balance = 1500 WHERE id = 1;\n";
        (void)executor.execute("UPDATE bank_accounts SET balance = 1500 WHERE id = 1;");
        std::cout << "  > INSERT INTO bank_accounts VALUES (3, 'Mallory', 9999);\n";
        (void)executor.execute("INSERT INTO bank_accounts VALUES (3, 'Mallory', 9999);");

        std::cout << "  [*] Visible in active transaction:\n";
        auto sel_active = executor.execute("SELECT * FROM bank_accounts;");
        klyrdb::SqlShell::print_result(sel_active.value(), std::cout);

        std::cout << "  > ROLLBACK;\n";
        (void)executor.execute("ROLLBACK;");

        std::cout << "  [+] State restored after ROLLBACK:\n";
        auto sel_rolled = executor.execute("SELECT * FROM bank_accounts;");
        klyrdb::SqlShell::print_result(sel_rolled.value(), std::cout);
        std::cout << "\n";

        // Step 5: Committed transaction followed by simulated crash
        std::cout << "[Step 5] Demonstration: Committed Transaction followed by Simulated Crash...\n";
        std::cout << "  > BEGIN;\n";
        (void)executor.execute("BEGIN;");
        std::cout << "  > INSERT INTO bank_accounts VALUES (4, 'Charlie', 2500);\n";
        (void)executor.execute("INSERT INTO bank_accounts VALUES (4, 'Charlie', 2500);");
        std::cout << "  > UPDATE bank_accounts SET balance = 1200 WHERE id = 1;\n";
        (void)executor.execute("UPDATE bank_accounts SET balance = 1200 WHERE id = 1;");
        std::cout << "  > COMMIT;\n";
        (void)executor.execute("COMMIT;");
        std::cout << "  [+] Transaction committed! Commit record is durably synced to WAL.\n";

        std::cout << "  [!] Simulating sudden power cut: discarding dirty buffer pool frames without writing to database file...\n";
        bpm.drop_frames_without_flush();
    }

    // Step 6: Crash Recovery
    std::cout << "\n[Step 6] Reopening database and running Crash Recovery...\n";
    {
        auto reopen_pf = klyrdb::PageFile::open(db_path).value();
        auto reopen_wal = klyrdb::WriteAheadLog::open_or_create(wal_path).value();
        klyrdb::BufferPoolManager reopen_bpm(*reopen_pf, 32);
        reopen_bpm.set_wal(reopen_wal.get());

        klyrdb::RecoveryManager rec_mgr(reopen_bpm, *reopen_wal);
        if (rec_mgr.needs_recovery()) {
            std::cout << "  [*] Pending WAL records detected! Executing RecoveryManager::recover()...\n";
            auto rec_res = rec_mgr.recover();
            if (rec_res.is_ok()) {
                const auto& st = rec_res.value();
                std::cout << "  [+] Recovery successful! Redone pages: " << st.pages_redone
                          << ", Undone pages: " << st.pages_undone
                          << ", Committed txns: " << st.committed_txns << "\n";
            }
        }

        // Step 7: Verify final database state
        std::cout << "\n[Step 7] Querying recovered database via SQL:\n";
        auto fsm2 = klyrdb::FreeSpaceMap::open_or_create(reopen_bpm).value();
        klyrdb::RecordManager rm2(reopen_bpm, fsm2.get());
        auto ic2 = klyrdb::IndexCatalog::open_or_create(reopen_bpm).value();
        auto tc2 = klyrdb::TableCatalog::open_or_create(reopen_bpm).value();
        klyrdb::SqlExecutor executor2(reopen_bpm, rm2, *tc2, *ic2);

        auto final_sel = executor2.execute("SELECT * FROM bank_accounts ORDER BY id ASC;");
        klyrdb::SqlShell::print_result(final_sel.value(), std::cout);
    }

    std::cout << "\n======================================================================\n";
    std::cout << "  KlyrDB Phase 5 Demonstration Completed Successfully!                \n";
    std::cout << "======================================================================\n";
    return 0;
}
