#include "klyrdb/database.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include <mutex>

namespace klyrdb {

class DatabaseImpl {
public:
    std::filesystem::path path;
    DatabaseOptions options;
    std::unique_ptr<PageFile> pf;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<WriteAheadLog> wal;
    std::unique_ptr<RecoveryManager> recovery;
    std::unique_ptr<FreeSpaceMap> fsm;
    std::unique_ptr<RecordManager> rm;
    std::unique_ptr<IndexCatalog> ic;
    std::unique_ptr<TableCatalog> tc;
    std::unique_ptr<TransactionManager> tm;
    std::unique_ptr<SqlExecutor> executor;
    std::mutex mutex;
    bool is_open{false};

    Result<ResultSet> execute(std::string_view sql) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!is_open || !executor) {
            return Status::io_error("Database is closed");
        }
        auto q_res = executor->execute(std::string(sql));
        if (!q_res.is_ok()) {
            return q_res.status();
        }
        return ResultSet(std::move(q_res.value()));
    }

    Status checkpoint() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!is_open || !bpm || !wal || !tm) {
            return Status::io_error("Database is closed or WAL is disabled");
        }
        CheckpointManager cp(*bpm, *wal, *tm);
        auto res = cp.checkpoint(CheckpointMode::Safe);
        if (!res.is_ok()) {
            return res.status();
        }
        return Status::ok();
    }

    Result<bool> integrity_check() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!is_open || !pf) {
            return Status::io_error("Database is closed");
        }
        if (bpm) {
            (void)bpm->flush_all_pages();
        }
        IntegrityChecker checker(*pf, wal.get());
        auto rep_res = checker.check_all();
        if (!rep_res.is_ok()) {
            return rep_res.status();
        }
        return rep_res.value().is_healthy;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!is_open) return;
        if (bpm) {
            (void)bpm->flush_all_pages();
        }
        if (pf) {
            (void)pf->sync();
        }
        executor.reset();
        tm.reset();
        tc.reset();
        ic.reset();
        rm.reset();
        fsm.reset();
        recovery.reset();
        bpm.reset();
        wal.reset();
        pf.reset();
        is_open = false;
    }
};

Database::Database(std::unique_ptr<DatabaseImpl> impl) : impl_(std::move(impl)) {}

Database::~Database() {
    close();
}

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& path, DatabaseOptions options) {
    auto impl = std::make_unique<DatabaseImpl>();
    impl->path = path;
    impl->options = options;

    std::filesystem::path wal_path = path.string() + ".wal";

    // 1. Open or create PageFile
    if (!std::filesystem::exists(path)) {
        auto pf_res = PageFile::create(path, options.lock_process);
        if (!pf_res.is_ok()) {
            return pf_res.status();
        }
        impl->pf = std::move(pf_res.value());
    } else {
        auto pf_res = PageFile::open(path, options.lock_process);
        if (!pf_res.is_ok()) {
            return pf_res.status();
        }
        impl->pf = std::move(pf_res.value());
    }

    // 2. Open or create WAL
    if (options.wal_enabled) {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        if (!wal_res.is_ok()) {
            return wal_res.status();
        }
        impl->wal = std::move(wal_res.value());
    }

    // 3. Initialize BufferPoolManager
    impl->bpm = std::make_unique<BufferPoolManager>(*impl->pf, options.buffer_pool_size);

    // 4. Run recovery if needed
    if (impl->wal) {
        impl->recovery = std::make_unique<RecoveryManager>(*impl->bpm, *impl->wal);
        if (impl->recovery->needs_recovery()) {
            auto rec_res = impl->recovery->recover();
            if (!rec_res.is_ok()) {
                return rec_res.status();
            }
        }
    }

    // 5. Open Catalogs & Managers
    auto fsm_res = FreeSpaceMap::open_or_create(*impl->bpm);
    if (!fsm_res.is_ok()) {
        return fsm_res.status();
    }
    impl->fsm = std::move(fsm_res.value());

    impl->rm = std::make_unique<RecordManager>(*impl->bpm, impl->fsm.get());

    auto ic_res = IndexCatalog::open_or_create(*impl->bpm);
    if (!ic_res.is_ok()) {
        return ic_res.status();
    }
    impl->ic = std::move(ic_res.value());

    auto tc_res = TableCatalog::open_or_create(*impl->bpm);
    if (!tc_res.is_ok()) {
        return tc_res.status();
    }
    impl->tc = std::move(tc_res.value());

    if (impl->wal) {
        impl->tm = std::make_unique<TransactionManager>(*impl->bpm, *impl->wal);
    }

    impl->executor = std::make_unique<SqlExecutor>(*impl->bpm, *impl->rm, *impl->tc, *impl->ic);
    if (impl->tm) {
        impl->executor->set_transaction_manager(impl->tm.get());
    }

    impl->is_open = true;
    return std::unique_ptr<Database>(new Database(std::move(impl)));
}

Result<std::unique_ptr<Connection>> Database::connect() {
    if (!is_open()) {
        return Status::io_error("Database is closed");
    }
    return std::make_unique<Connection>(*this);
}

Result<ResultSet> Database::execute(std::string_view sql) {
    if (!impl_) {
        return Status::io_error("Database handle is null");
    }
    return impl_->execute(sql);
}

Status Database::checkpoint() {
    if (!impl_) {
        return Status::io_error("Database handle is null");
    }
    return impl_->checkpoint();
}

Result<bool> Database::integrity_check() {
    if (!impl_) {
        return Status::io_error("Database handle is null");
    }
    return impl_->integrity_check();
}

void Database::close() {
    if (impl_) {
        impl_->close();
    }
}

const std::filesystem::path& Database::path() const noexcept {
    static const std::filesystem::path empty_path;
    return impl_ ? impl_->path : empty_path;
}

bool Database::is_open() const noexcept {
    return impl_ && impl_->is_open;
}

} // namespace klyrdb
