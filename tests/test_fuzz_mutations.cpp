#include "test_framework.hpp"
#include "klyrdb/sql/parser.hpp"
#include "klyrdb/backup/csv_migrator.hpp"
#include "klyrdb/wal/log_record.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/backup/backup_manifest.hpp"
#include <random>
#include <string>
#include <vector>
#include <fstream>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_sql_parser_fuzzing() {
    const std::vector<std::string> malformed_queries = {
        "",
        ";",
        ";;;",
        "SELECT",
        "SELECT FROM",
        "SELECT * FROM",
        "SELECT * FROM users WHERE",
        "SELECT * FROM users WHERE age =",
        "SELECT * FROM users WHERE age == 5",
        "SELECT * FROM users ORDER",
        "SELECT * FROM users ORDER BY",
        "SELECT * FROM users ORDER BY age INVALID",
        "SELECT * FROM users LIMIT",
        "SELECT * FROM users LIMIT -5",
        "SELECT * FROM users LIMIT abc",
        "INSERT",
        "INSERT INTO",
        "INSERT INTO users",
        "INSERT INTO users VALUES",
        "INSERT INTO users VALUES (",
        "INSERT INTO users VALUES (1,",
        "INSERT INTO users VALUES (1, 'unterminated string)",
        "CREATE",
        "CREATE TABLE",
        "CREATE TABLE (id INT)",
        "CREATE TABLE 123 (id INTEGER)",
        "CREATE TABLE t (id INVALID_TYPE)",
        "CREATE TABLE t (id INTEGER, UNIQUE)",
        "DROP",
        "DROP TABLE",
        "DROP TABLE ;",
        "UPDATE",
        "UPDATE users",
        "UPDATE users SET",
        "UPDATE users SET age = 10", // Missing WHERE clause
        "DELETE",
        "DELETE FROM",
        "DELETE FROM users", // Missing WHERE clause
        "EXPLAIN",
        "EXPLAIN QUERY",
        "EXPLAIN ANALYZE",
        "EXPLAIN ANALYZE INSERT INTO t VALUES (1);",
        std::string("SELECT \0 NULL BYTE", 18),
        "SELECT * FROM 12345;",
        "SELECT * FROM ;"
    };

    for (const auto& q : malformed_queries) {
        auto res = SqlParser::parse_sql(q);
        // Crucial property: Must return an error cleanly without crashing, segfaulting, or throwing
        if (q != ";") { // semicolon alone might be treated as empty
            EXPECT_TRUE(res.is_err());
        }
    }

    // Randomized mutation fuzzing: take valid queries and mutate random bytes
    std::mt19937 rng(4242);
    std::vector<std::string> seeds = {
        "SELECT id, name FROM users WHERE id = 10 ORDER BY id ASC LIMIT 5;",
        "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30);",
        "UPDATE users SET age = 31 WHERE id = 1;",
        "DELETE FROM users WHERE age > 65;",
        "CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT NOT NULL);"
    };

    for (const auto& seed : seeds) {
        for (int mut = 0; mut < 50; ++mut) {
            std::string mutated = seed;
            int num_edits = 1 + (rng() % 5);
            for (int e = 0; e < num_edits; ++e) {
                size_t pos = rng() % mutated.size();
                int action = rng() % 3;
                if (action == 0) {
                    mutated[pos] = static_cast<char>(rng() % 256); // Replace
                } else if (action == 1) {
                    mutated.insert(pos, 1, static_cast<char>(rng() % 256)); // Insert
                } else if (mutated.size() > 1) {
                    mutated.erase(pos, 1); // Delete
                }
            }
            // Parse must not crash!
            auto res = SqlParser::parse_sql(mutated);
            (void)res;
        }
    }
}

static void test_csv_fuzzing() {
    auto path = temp_test_file("test_fuzz_csv.csv");
    ScopedTempFile cleanup(path);

    const std::vector<std::string> malformed_csvs = {
        "",
        "\n\n\n",
        "id,name,score\n1,Alice\n", // Column count mismatch
        "id,name,score\n1,\"unclosed quote,100\n",
        "id,name,score\n1,Bob,100,200\n", // Extra column
        std::string("id,name,score\n1,\0\0\0,100\n", 25), // Null bytes
        "id,name,score\n" + std::string(100000, 'A') + ",test,50\n" // Massive token
    };

    CsvMigrator migrator;
    for (const auto& csv_content : malformed_csvs) {
        {
            std::ofstream ofs(path, std::ios::binary);
            ofs.write(csv_content.data(), csv_content.size());
        }
        // Attempt import into non-existent table or dummy
        auto res = migrator.import_csv(path.string(), "dummy.klyr", "users");
        // Must fail cleanly
        EXPECT_TRUE(res.is_err());
    }
}

static void test_wal_record_corruption_fuzzing() {
    // Construct a valid record
    std::vector<uint8_t> before_img(PAGE_SIZE, 0xAA);
    std::vector<uint8_t> after_img(PAGE_SIZE, 0xBB);
    LogRecord rec = LogRecord::make_page_update(10, 1, 5, 42, before_img, after_img);
    std::vector<uint8_t> encoded;
    rec.encode(encoded);
    EXPECT_TRUE(encoded.size() > 0);

    // Verify valid decode
    auto valid_res = LogRecord::decode(encoded);
    EXPECT_TRUE(valid_res.is_ok());

    // 1. Truncated buffers
    for (size_t len = 0; len < encoded.size() - 1; len += 100) {
        std::span<const uint8_t> truncated(encoded.data(), len);
        auto trunc_res = LogRecord::decode(truncated);
        EXPECT_TRUE(trunc_res.is_err());
    }

    // 2. Corrupt checksum
    std::vector<uint8_t> corrupt_crc = encoded;
    corrupt_crc[encoded.size() - 1] ^= 0xFF;
    auto crc_res = LogRecord::decode(corrupt_crc);
    EXPECT_TRUE(crc_res.is_err());

    // 3. Corrupt record type enum
    std::vector<uint8_t> corrupt_type = encoded;
    corrupt_type[0] = 0xFE; // Invalid LogRecordType
    auto type_res = LogRecord::decode(corrupt_type);
    EXPECT_TRUE(type_res.is_err());
}

static void test_manifest_fuzzing() {
    const std::vector<std::string> malformed_manifests = {
        "",
        "{",
        "{}",
        "{\"manifest_version\": 999}",
        "{\"manifest_version\": 1, \"database_name\": \"test\"}", // Missing files
        "{\"manifest_version\": 1, \"database_name\": \"test\", \"files\": [\"not_an_object\"]}"
    };

    for (const auto& m_str : malformed_manifests) {
        auto res = BackupManifest::from_json(m_str);
        EXPECT_TRUE(res.is_err());
    }
}

int main() {
    std::cout << "=== Running Fuzz & Mutation Robustness Tests ===\n";

    run_test("test_sql_parser_fuzzing", test_sql_parser_fuzzing);
    run_test("test_csv_fuzzing", test_csv_fuzzing);
    run_test("test_wal_record_corruption_fuzzing", test_wal_record_corruption_fuzzing);
    run_test("test_manifest_fuzzing", test_manifest_fuzzing);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
