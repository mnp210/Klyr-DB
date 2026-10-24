#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_not_null_constraints() {
    auto path = temp_test_file("test_not_null");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    // 1. CREATE TABLE with NOT NULL and nullable columns
    auto res_ct = executor.execute("CREATE TABLE products (id INTEGER NOT NULL, name TEXT NOT NULL, description TEXT, price INTEGER);");
    EXPECT_TRUE(res_ct.is_ok());

    // 2. Insert valid row with all values
    auto ins1 = executor.execute("INSERT INTO products VALUES (1, 'Widget', 'Useful tool', 100);");
    EXPECT_TRUE(ins1.is_ok());

    // 3. Insert valid row with NULL in nullable column
    auto ins2 = executor.execute("INSERT INTO products VALUES (2, 'Gadget', NULL, NULL);");
    EXPECT_TRUE(ins2.is_ok());

    // 4. Insert row with empty string in NOT NULL text column - MUST SUCCEED (empty string != NULL)
    auto ins3 = executor.execute("INSERT INTO products VALUES (3, '', 'No name widget', 0);");
    EXPECT_TRUE(ins3.is_ok());

    // 5. Insert row with 0 in NOT NULL integer column - MUST SUCCEED (0 != NULL)
    auto ins4 = executor.execute("INSERT INTO products VALUES (0, 'ZeroPrice', 'Free item', 0);");
    EXPECT_TRUE(ins4.is_ok());

    // 6. Named insert omitting nullable columns - MUST SUCCEED
    auto ins5 = executor.execute("INSERT INTO products (id, name) VALUES (5, 'CompactWidget');");
    EXPECT_TRUE(ins5.is_ok());

    // 7. Named insert omitting NOT NULL column - MUST FAIL
    auto ins6 = executor.execute("INSERT INTO products (id, description) VALUES (6, 'Missing name');");
    EXPECT_TRUE(ins6.is_err());
    EXPECT_EQ(ins6.status().code(), StatusCode::ConstraintViolation);

    // 8. Positional insert with explicit NULL in NOT NULL column - MUST FAIL
    auto ins7 = executor.execute("INSERT INTO products VALUES (NULL, 'NullID', 'Desc', 50);");
    EXPECT_TRUE(ins7.is_err());
    EXPECT_EQ(ins7.status().code(), StatusCode::ConstraintViolation);

    auto ins8 = executor.execute("INSERT INTO products VALUES (8, NULL, 'NullName', 50);");
    EXPECT_TRUE(ins8.is_err());
    EXPECT_EQ(ins8.status().code(), StatusCode::ConstraintViolation);

    // 9. UPDATE setting NOT NULL column to NULL - MUST FAIL
    auto upd1 = executor.execute("UPDATE products SET name = NULL WHERE id = 1;");
    EXPECT_TRUE(upd1.is_err());
    EXPECT_EQ(upd1.status().code(), StatusCode::ConstraintViolation);

    // 10. UPDATE setting nullable column to NULL - MUST SUCCEED
    auto upd2 = executor.execute("UPDATE products SET description = NULL WHERE id = 1;");
    EXPECT_TRUE(upd2.is_ok());

    // Verify row counts and values
    auto sel = executor.execute("SELECT * FROM products;");
    EXPECT_TRUE(sel.is_ok());
    EXPECT_EQ(sel.value().row_count(), 5ULL);
}

static void test_primary_key_constraints() {
    auto path = temp_test_file("test_pk");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    // 1. Column-level PRIMARY KEY
    auto ct1 = executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT);");
    EXPECT_TRUE(ct1.is_ok());

    // Verify backing index exists in TableCatalog and IndexCatalog
    auto tbl = tc->get_table("users").value();
    EXPECT_TRUE(tbl.is_primary_key("id"));
    EXPECT_TRUE(tbl.find_index_for_column("id") != nullptr);
    EXPECT_TRUE(tbl.find_index_for_column("id")->is_unique);

    // 2. Insert valid row
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, 'Alice');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 'Bob');").is_ok());

    // 3. Reject duplicate primary key
    auto dup = executor.execute("INSERT INTO users VALUES (1, 'DuplicateAlice');");
    EXPECT_TRUE(dup.is_err());
    EXPECT_EQ(dup.status().code(), StatusCode::ConstraintViolation);

    // 4. Reject NULL primary key
    auto null_pk1 = executor.execute("INSERT INTO users VALUES (NULL, 'NullUser');");
    EXPECT_TRUE(null_pk1.is_err());
    EXPECT_EQ(null_pk1.status().code(), StatusCode::ConstraintViolation);

    // 5. Reject omitted primary key in named insert
    auto null_pk2 = executor.execute("INSERT INTO users (username) VALUES ('OmittedPK');");
    EXPECT_TRUE(null_pk2.is_err());
    EXPECT_EQ(null_pk2.status().code(), StatusCode::ConstraintViolation);

    // 6. Reject UPDATE setting primary key to duplicate
    auto upd_dup = executor.execute("UPDATE users SET id = 1 WHERE id = 2;");
    EXPECT_TRUE(upd_dup.is_err());
    EXPECT_EQ(upd_dup.status().code(), StatusCode::ConstraintViolation);

    // 7. Reject UPDATE setting primary key to NULL
    auto upd_null = executor.execute("UPDATE users SET id = NULL WHERE id = 1;");
    EXPECT_TRUE(upd_null.is_err());
    EXPECT_EQ(upd_null.status().code(), StatusCode::ConstraintViolation);

    // 8. Valid UPDATE of primary key to unused value
    EXPECT_TRUE(executor.execute("UPDATE users SET id = 10 WHERE id = 2;").is_ok());

    // Verify row 10 exists
    auto sel = executor.execute("SELECT * FROM users WHERE id = 10;");
    EXPECT_TRUE(sel.is_ok());
    EXPECT_EQ(sel.value().row_count(), 1ULL);

    // 9. Reject table with multiple primary keys
    auto ct_multi = executor.execute("CREATE TABLE bad_table (id1 INTEGER PRIMARY KEY, id2 INTEGER PRIMARY KEY);");
    EXPECT_TRUE(ct_multi.is_err());
    EXPECT_EQ(ct_multi.status().code(), StatusCode::ConstraintViolation);

    // 10. Table-level PRIMARY KEY
    auto ct2 = executor.execute("CREATE TABLE accounts (acc_num INTEGER, balance INTEGER, PRIMARY KEY (acc_num));");
    EXPECT_TRUE(ct2.is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (1001, 500);").is_ok());
    auto acc_dup = executor.execute("INSERT INTO accounts VALUES (1001, 700);");
    EXPECT_TRUE(acc_dup.is_err());
    EXPECT_EQ(acc_dup.status().code(), StatusCode::ConstraintViolation);
}

static void test_unique_constraints() {
    auto path = temp_test_file("test_unique");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    // Column-level UNIQUE and table-level UNIQUE
    auto ct = executor.execute("CREATE TABLE members (id INTEGER PRIMARY KEY, email TEXT UNIQUE, phone TEXT, UNIQUE (phone));");
    EXPECT_TRUE(ct.is_ok());

    // 1. Insert unique rows
    EXPECT_TRUE(executor.execute("INSERT INTO members VALUES (1, 'alice@example.com', '111-222');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO members VALUES (2, 'bob@example.com', '333-444');").is_ok());

    // 2. Reject duplicate email
    auto dup_email = executor.execute("INSERT INTO members VALUES (3, 'alice@example.com', '555-666');");
    EXPECT_TRUE(dup_email.is_err());
    EXPECT_EQ(dup_email.status().code(), StatusCode::ConstraintViolation);

    // 3. Reject duplicate phone
    auto dup_phone = executor.execute("INSERT INTO members VALUES (4, 'dave@example.com', '111-222');");
    EXPECT_TRUE(dup_phone.is_err());
    EXPECT_EQ(dup_phone.status().code(), StatusCode::ConstraintViolation);

    // 4. SQL standard: Multiple NULL values are permitted in UNIQUE columns
    EXPECT_TRUE(executor.execute("INSERT INTO members VALUES (5, NULL, '777-888');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO members VALUES (6, NULL, '999-000');").is_ok());

    // Multiple NULL phones are also permitted
    EXPECT_TRUE(executor.execute("INSERT INTO members VALUES (7, 'user7@example.com', NULL);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO members VALUES (8, 'user8@example.com', NULL);").is_ok());

    // 5. UPDATE violating UNIQUE constraint
    auto upd_dup = executor.execute("UPDATE members SET email = 'bob@example.com' WHERE id = 1;");
    EXPECT_TRUE(upd_dup.is_err());
    EXPECT_EQ(upd_dup.status().code(), StatusCode::ConstraintViolation);

    // 6. UPDATE setting UNIQUE column to NULL is allowed
    EXPECT_TRUE(executor.execute("UPDATE members SET email = NULL WHERE id = 2;").is_ok());

    // Total rows = 6
    auto sel = executor.execute("SELECT * FROM members;");
    EXPECT_TRUE(sel.is_ok());
    EXPECT_EQ(sel.value().row_count(), 6ULL);
}

static void test_foreign_key_constraints() {
    auto path = temp_test_file("test_fk");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    // 1. Cannot create FK referencing non-existent table
    auto bad_tbl = executor.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, cust_id INTEGER REFERENCES non_existent(id));");
    EXPECT_TRUE(bad_tbl.is_err());
    EXPECT_EQ(bad_tbl.status().code(), StatusCode::TableNotFound);

    // Create parent table: departments
    EXPECT_TRUE(executor.execute("CREATE TABLE departments (dept_id INTEGER PRIMARY KEY, dept_name TEXT UNIQUE);").is_ok());

    // 2. Cannot create FK referencing non-existent column in existing table
    auto bad_col = executor.execute("CREATE TABLE employees (id INTEGER PRIMARY KEY, d_id INTEGER REFERENCES departments(non_col));");
    EXPECT_TRUE(bad_col.is_err());
    EXPECT_EQ(bad_col.status().code(), StatusCode::ColumnNotFound);

    // 3. Cannot create FK with type mismatch
    auto type_mismatch = executor.execute("CREATE TABLE employees (id INTEGER PRIMARY KEY, d_name INTEGER REFERENCES departments(dept_name));");
    EXPECT_TRUE(type_mismatch.is_err());
    EXPECT_EQ(type_mismatch.status().code(), StatusCode::TypeMismatch);

    // 4. Create valid child table with column-level REFERENCES and table-level FOREIGN KEY
    auto ct_emp = executor.execute(
        "CREATE TABLE employees ("
        "  emp_id INTEGER PRIMARY KEY,"
        "  emp_name TEXT NOT NULL,"
        "  dept_id INTEGER,"
        "  FOREIGN KEY (dept_id) REFERENCES departments(dept_id)"
        ");"
    );
    EXPECT_TRUE(ct_emp.is_ok());

    // Insert departments
    EXPECT_TRUE(executor.execute("INSERT INTO departments VALUES (10, 'Engineering');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO departments VALUES (20, 'Marketing');").is_ok());

    // 5. Valid child inserts
    EXPECT_TRUE(executor.execute("INSERT INTO employees VALUES (101, 'Alice', 10);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO employees VALUES (102, 'Bob', 20);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO employees VALUES (103, 'Charlie', 10);").is_ok());

    // 6. Child insert with NULL foreign key is valid
    EXPECT_TRUE(executor.execute("INSERT INTO employees VALUES (104, 'Dave', NULL);").is_ok());

    // 7. Child insert with non-existent parent key - MUST FAIL
    auto bad_ins = executor.execute("INSERT INTO employees VALUES (105, 'Eve', 99);");
    EXPECT_TRUE(bad_ins.is_err());
    EXPECT_EQ(bad_ins.status().code(), StatusCode::ConstraintViolation);

    // 8. Child update to non-existent parent key - MUST FAIL
    auto bad_upd = executor.execute("UPDATE employees SET dept_id = 99 WHERE emp_id = 101;");
    EXPECT_TRUE(bad_upd.is_err());
    EXPECT_EQ(bad_upd.status().code(), StatusCode::ConstraintViolation);

    // 9. Child update to valid parent key - MUST SUCCEED
    EXPECT_TRUE(executor.execute("UPDATE employees SET dept_id = 20 WHERE emp_id = 101;").is_ok());

    // 10. RESTRICT on parent DELETE: cannot delete department 20 because Alice & Bob reference it
    auto del_parent = executor.execute("DELETE FROM departments WHERE dept_id = 20;");
    EXPECT_TRUE(del_parent.is_err());
    EXPECT_EQ(del_parent.status().code(), StatusCode::ConstraintViolation);

    // 11. RESTRICT on parent UPDATE: cannot change department 20's dept_id because employees reference it
    auto upd_parent = executor.execute("UPDATE departments SET dept_id = 25 WHERE dept_id = 20;");
    EXPECT_TRUE(upd_parent.is_err());
    EXPECT_EQ(upd_parent.status().code(), StatusCode::ConstraintViolation);

    // Delete or reassign referencing employees
    EXPECT_TRUE(executor.execute("DELETE FROM employees WHERE dept_id = 20;").is_ok());

    // Now parent delete of department 20 MUST SUCCEED
    EXPECT_TRUE(executor.execute("DELETE FROM departments WHERE dept_id = 20;").is_ok());
}

static void test_ddl_and_index_protection() {
    auto path = temp_test_file("test_protection");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE parent_tbl (p_id INTEGER PRIMARY KEY, code TEXT UNIQUE);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE TABLE child_tbl (c_id INTEGER PRIMARY KEY, p_id INTEGER REFERENCES parent_tbl(p_id));").is_ok());

    // 1. Cannot drop PK index
    auto drop_pk_idx = executor.execute("DROP INDEX pk_parent_tbl_p_id;");
    EXPECT_TRUE(drop_pk_idx.is_err());
    EXPECT_EQ(drop_pk_idx.status().code(), StatusCode::ConstraintViolation);

    // 2. Cannot drop UNIQUE index
    auto drop_uniq_idx = executor.execute("DROP INDEX uk_parent_tbl_code;");
    EXPECT_TRUE(drop_uniq_idx.is_err());
    EXPECT_EQ(drop_uniq_idx.status().code(), StatusCode::ConstraintViolation);

    // 3. Cannot drop referenced parent table
    auto drop_parent = executor.execute("DROP TABLE parent_tbl;");
    EXPECT_TRUE(drop_parent.is_err());
    EXPECT_EQ(drop_parent.status().code(), StatusCode::ConstraintViolation);

    // 4. Can drop child table
    EXPECT_TRUE(executor.execute("DROP TABLE child_tbl;").is_ok());

    // 5. Once child table is dropped, parent table CAN be dropped
    EXPECT_TRUE(executor.execute("DROP TABLE parent_tbl;").is_ok());
}

static void test_constraint_persistence_and_reopen() {
    auto path = temp_test_file("test_reopen");
    ScopedTempFile cleanup(path);

    // Session 1: Create schema with constraints and insert initial data
    {
        auto pf = PageFile::create(path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();

        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE teams (team_id INTEGER PRIMARY KEY, team_name TEXT NOT NULL UNIQUE);").is_ok());
        EXPECT_TRUE(executor.execute("CREATE TABLE players (player_id INTEGER PRIMARY KEY, name TEXT NOT NULL, team_id INTEGER REFERENCES teams(team_id));").is_ok());

        EXPECT_TRUE(executor.execute("INSERT INTO teams VALUES (1, 'Alpha');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO players VALUES (10, 'John', 1);").is_ok());

        bpm.flush_all_pages();
    }

    // Session 2: Reopen database and verify all constraints are still actively enforced!
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();

        SqlExecutor executor(bpm, rm, *tc, *ic);

        // 1. Verify PK constraint enforcement on reopen
        auto dup_pk = executor.execute("INSERT INTO teams VALUES (1, 'Beta');");
        EXPECT_TRUE(dup_pk.is_err());
        EXPECT_EQ(dup_pk.status().code(), StatusCode::ConstraintViolation);

        // 2. Verify UNIQUE constraint enforcement on reopen
        auto dup_uniq = executor.execute("INSERT INTO teams VALUES (2, 'Alpha');");
        EXPECT_TRUE(dup_uniq.is_err());
        EXPECT_EQ(dup_uniq.status().code(), StatusCode::ConstraintViolation);

        // 3. Verify NOT NULL constraint enforcement on reopen
        auto null_val = executor.execute("INSERT INTO teams VALUES (3, NULL);");
        EXPECT_TRUE(null_val.is_err());
        EXPECT_EQ(null_val.status().code(), StatusCode::ConstraintViolation);

        // 4. Verify FOREIGN KEY referential integrity on reopen
        auto bad_fk = executor.execute("INSERT INTO players VALUES (11, 'Mark', 999);");
        EXPECT_TRUE(bad_fk.is_err());
        EXPECT_EQ(bad_fk.status().code(), StatusCode::ConstraintViolation);

        // 5. Verify RESTRICT on parent delete on reopen
        auto del_parent = executor.execute("DELETE FROM teams WHERE team_id = 1;");
        EXPECT_TRUE(del_parent.is_err());
        EXPECT_EQ(del_parent.status().code(), StatusCode::ConstraintViolation);
    }
}

int main() {
    std::cout << "Running Constraints & Relational Integrity Tests...\n";
    run_test("NOT NULL Constraints", test_not_null_constraints);
    run_test("PRIMARY KEY Constraints", test_primary_key_constraints);
    run_test("UNIQUE Constraints", test_unique_constraints);
    run_test("FOREIGN KEY Constraints", test_foreign_key_constraints);
    run_test("DDL & Index Protection", test_ddl_and_index_protection);
    run_test("Constraint Persistence and Reopen", test_constraint_persistence_and_reopen);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
