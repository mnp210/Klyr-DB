# KlyrDB Versioning & Compatibility Policy

## 1. Versioning Architecture

KlyrDB maintains a strict distinction between three distinct version indicators:

1. **Software Engine Version (`Version`)**:
   - Follows Semantic Versioning (`MAJOR.MINOR.PATCH`).
   - Current release: **`0.12.0`**.
   - Incremented across public API additions, behavioral refinements, and release cycles.
   - Queryable via `klyrdb::version()` in C++ and `klyrdb --version` via CLI.

2. **On-Disk Storage Format Version (`storage_format_version`)**:
   - Current version: **`1`**.
   - Stored in Page 0 (Database Metadata page) at byte offset 8 (`format_version`).
   - Determines slotted page layout, 24-byte page headers, Big-Endian encoding, CRC-32 checksums, B+ tree node structures, and table catalog schema definitions.
   - Preserves 100% backward compatibility with database files created in earlier phases (e.g. `demo_phase5.kdb`).

3. **Physical Backup Format Version (`backup_format_version`)**:
   - Current version: **`1`**.
   - Stored in `manifest.json` under `backup_version`.
   - Specifies snapshot component geometry, bit-level CRC-32 hashes, and LSN boundaries.

---

## 2. Backward Compatibility Guarantees

- **Database Files**: KlyrDB engines of version `0.12.0` can open, inspect, query, checkpoint, and modify any database file with storage format version `1`.
- **Physical Backups**: Backups generated with format version `1` can be verified and atomically restored by any compatible engine.
- **Unsupported Versions**: If a database file with an unsupported or future storage version is supplied, the engine immediately rejects opening with `StatusCode::CorruptPage` or `StatusCode::InvalidArgument` rather than corrupting data.
