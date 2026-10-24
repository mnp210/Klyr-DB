#include "klyrdb/concurrency/lock_manager.hpp"

namespace klyrdb {

LockManager& LockManager::instance() {
    static LockManager inst;
    return inst;
}

std::shared_ptr<DatabaseLock> LockManager::get_lock(const std::filesystem::path& db_path, bool enable_process_lock) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::string key = std::filesystem::absolute(db_path).lexically_normal().string();

    auto it = locks_.find(key);
    if (it != locks_.end()) {
        return it->second;
    }

    auto lock = std::make_shared<DatabaseLock>(db_path, enable_process_lock);
    locks_[key] = lock;
    return lock;
}

void LockManager::clean_unused_locks() {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = locks_.begin(); it != locks_.end();) {
        if (it->second.use_count() == 1 &&
            !it->second->is_exclusively_locked() &&
            it->second->shared_reader_count() == 0) {
            it = locks_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace klyrdb
