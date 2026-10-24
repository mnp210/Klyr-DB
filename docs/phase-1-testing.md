# KlyrDB Phase 1 — Testing & Verification Strategy

## 1. Testing Philosophy

KlyrDB Phase 1 uses a hermetic, zero-external-dependency test harness. All tests compile with standard C++20 and execute via CMake's `ctest` or direct binaries.

Key guarantees:
- **Zero Network/Database Dependencies**: No external server, daemon, or network access required.
- **Safety**: Tests run exclusively against isolated temporary files in the OS temporary directory (`std::filesystem::temp_directory_path()`).
- **RAII Teardown**: The `ScopedTempFile` wrapper ensures all test artifacts are removed upon test completion or failure.
- **Hermetic Executables**: Each test suite is an independent executable with distinct reporting.

---

## 2. Test Suites

### 2.1 `test_page` (Page Unit Tests)
- **Endian Roundtrip**: Validates explicit Big-Endian encoding/decoding of 16-bit, 32-bit, 64-bit integers and floating-point values via bit shifting.
- **Checksum Properties**: Validates CRC-32 deterministic calculation, non-zero results, and single-bit corruption sensitivity.
- **Page Initialization**: Asserts default header invariants (magic, version, zero payload size, page size = 4096, max payload = 4072).
- **Page Serialization & Deserialization**: Asserts that an in-memory page serializes to exactly 4096 bytes and deserializes with byte-for-byte fidelity.
- **Empty Payload**: Tests 0-byte payload boundary condition.
- **Maximum Payload**: Fills 4072 bytes of payload data and tests exact boundary packing and verification.
- **Payload Overflow Rejection**: Asserts that payloads > 4072 bytes and out-of-bounds offset writes fail with `StatusCode::PayloadOverflow`.
- **Corrupted Checksum Rejection**: Modifies a payload byte in a serialized page and asserts that `deserialize` returns `StatusCode::ChecksumMismatch`.
- **Incorrect Magic Rejection**: Tampers with magic signature bytes and asserts rejection with `StatusCode::InvalidMagic`.
- **Unsupported Version Rejection**: Changes format version to 99 and asserts rejection with `StatusCode::UnsupportedVersion`.

### 2.2 `test_page_file` (Storage Engine Integration Tests)
- **File Create and Reopen**: Validates initial creation of Page 0 and successful reopening.
- **Reject Existing File on Create**: Confirms non-destructive file creation fails with `StatusCode::FileAlreadyExists` without overwriting data.
- **Open Nonexistent File**: Confirms opening missing paths fails with `StatusCode::FileNotFound`.
- **Multiple Page Appends and Reads**: Appends 5 distinct pages, checks sequential page ID generation, reads back all pages, and tests out-of-range reads.
- **Detect Truncated Files**: Tests rejection of 0-byte files and non-page-aligned files (< 4096 bytes) with `StatusCode::TruncatedFile`.
- **Update Existing Page**: Validates in-place mutation of allocated pages via `write_page`.

### 2.3 `test_persistence` (End-to-End Persistence & Corruption Tests)
- **Full Roundtrip Persistence**: Appends 10 pages with padding, closes the file stream, destroys all in-memory state, reopens in a fresh `PageFile` instance, and validates data fidelity.
- **Disk Corruption Detection**: Mutates a byte in the database file on disk using a raw file stream, reopens with KlyrDB, and validates that `read_page` fails with `StatusCode::ChecksumMismatch`.
- **Record Persistence in Page**: Serializes a complex 6-field `Record` (Null, Int32, Int64, Float64, String, Bytes), writes to a database page on disk, reopens, and validates deserialized field equality.

---

## 3. How to Run the Tests

### Using CMake & CTest
```powershell
# Configure (using Visual Studio 2026 or Ninja)
cmake -B build -S .

# Build all targets and tests
cmake --build build --config Debug

# Run all tests via CTest
ctest --test-dir build -C Debug --output-on-failure
```

### Running Test Binaries Directly
```powershell
.\build\tests\Debug\test_page.exe
.\build\tests\Debug\test_page_file.exe
.\build\tests\Debug\test_persistence.exe
```
