# KlyrDB Fuzz Testing & Fault-Injection Methodology

## 1. Overview

To guarantee absolute crash resistance and memory safety when exposed to untrusted, malformed, or malicious input, Phase 11 implements comprehensive fuzzing and mutation testing (`tests/test_fuzz_mutations.cpp`).

---

## 2. Query Parser Mutation Fuzzing

The SQL parser must never crash, throw unhandled exceptions, leak memory, or cause undefined behavior regardless of the input provided.

### Test Categories:
1. **Syntactic Boundary Vectors**:
   - Truncated statements (`SELECT`, `INSERT INTO`, `UPDATE users SET`).
   - Missing clauses (`UPDATE users SET age = 10` missing WHERE, `DELETE FROM users`).
   - Invalid keywords and structural tokens.
   - Semicolon-only statements and empty inputs.
2. **Byte-Level Null Injection**:
   - Raw embedded `\0` bytes inside strings and identifier positions (`SELECT \0 NULL BYTE`).
3. **Randomized Bit-Flip & Byte Mutation**:
   - Takes valid SQL templates (`SELECT id, name FROM users WHERE id = 10;`, `INSERT INTO users VALUES (1, 'alice');`) and applies random byte replacement, deletion, and insertion across 200 pseudo-random iterations.
   - Verifies that `SqlParser::parse_sql()` either produces a valid AST or returns `Status::syntax_error()` cleanly.

---

## 3. Storage Corruption & Partial Page Fuzzing

1. **Header Bit-Flip Injections**:
   - Flips arbitrary bits in slotted page headers (slot counts, free space offsets, page LSNs).
   - Verifies that page deserialization catches corrupt headers via `Status::corrupt_page()` or checksum validation rather than out-of-bounds pointer reads.
2. **Truncated Page Boundaries**:
   - Feeds partial byte buffers (< 4096 bytes) to `Page::deserialize()`.
   - Confirms deterministic `Status::truncated_file()` return.

---

## 4. WAL Record Bit-Flipping & Record CRC Invalidation

1. **Log Record Truncation**:
   - Truncates log files mid-record.
   - Confirms that recovery aborts or handles incomplete log records safely without looping infinitely.
2. **Payload Mutation**:
   - Mutates before/after image payloads in WAL records.
   - Validates that recovery validates record integrity and detects mismatches.
