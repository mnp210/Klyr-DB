#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <source_location>

namespace klyrdb::testing {

inline int g_failed_tests = 0;
inline int g_passed_tests = 0;

#define EXPECT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            klyrdb::testing::g_failed_tests++; \
            return; \
        } \
    } while (0)

#define EXPECT_FALSE(cond) \
    do { \
        if ((cond)) { \
            std::cerr << "  FAILED: NOT(" #cond ") at " << __FILE__ << ":" << __LINE__ << "\n"; \
            klyrdb::testing::g_failed_tests++; \
            return; \
        } \
    } while (0)

#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "  FAILED: " #a " == " #b " (" << (a) << " != " << (b) << ") at " \
                      << __FILE__ << ":" << __LINE__ << "\n"; \
            klyrdb::testing::g_failed_tests++; \
            return; \
        } \
    } while (0)

#define EXPECT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            std::cerr << "  FAILED: " #a " != " #b " at " \
                      << __FILE__ << ":" << __LINE__ << "\n"; \
            klyrdb::testing::g_failed_tests++; \
            return; \
        } \
    } while (0)

inline void run_test(const std::string& name, const std::function<void()>& test_fn) {
    const int prev_failed = g_failed_tests;
    std::cout << "[ RUN      ] " << name << std::endl;
    try {
        test_fn();
        if (g_failed_tests == prev_failed) {
            std::cout << "[       OK ] " << name << std::endl;
            g_passed_tests++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  EXCEPTION in " << name << ": " << e.what() << "\n";
        g_failed_tests++;
    }
}

/// Helper to generate a unique temporary test path in the OS temp directory
inline std::filesystem::path temp_test_file(const std::string& prefix) {
    auto base = std::filesystem::temp_directory_path();
    static uint64_t counter = 0;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return base / (prefix + "_" + std::to_string(now) + "_" + std::to_string(++counter) + ".kdb");
}

/// RAII helper that guarantees removal of a temporary file on scope exit
class ScopedTempFile {
public:
    explicit ScopedTempFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedTempFile() {
        std::error_code ec;
        if (std::filesystem::exists(path_, ec)) {
            std::filesystem::remove(path_, ec);
        }
        std::filesystem::path wal_path = path_.string() + ".wal";
        if (std::filesystem::exists(wal_path, ec)) {
            std::filesystem::remove(wal_path, ec);
        }
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ScopedTempFile(ScopedTempFile&&) = default;
    ScopedTempFile& operator=(ScopedTempFile&&) = default;

private:
    std::filesystem::path path_;
};

/// RAII helper that guarantees removal of a temporary directory on scope exit
class ScopedTempDir {
public:
    explicit ScopedTempDir(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedTempDir() {
        std::error_code ec;
        if (std::filesystem::exists(path_, ec)) {
            std::filesystem::remove_all(path_, ec);
        }
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = default;
    ScopedTempDir& operator=(ScopedTempDir&&) = default;

private:
    std::filesystem::path path_;
};

} // namespace klyrdb::testing
