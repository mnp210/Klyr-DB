# KlyrDB Release Engineering & Packaging Process

## 1. Overview

KlyrDB provides fully automated, reproducible release packaging scripts that build optimized Release binaries, run the full 50-suite CTest verification matrix, package headers and binaries into a distribution archive, and emit cryptographic SHA-256 checksums.

---

## 2. Release Scripts

### Windows x64
```powershell
powershell -ExecutionPolicy Bypass -File scripts/package_release.ps1 -Version 0.12.0 -BuildType Release
```

### Linux x64
```bash
./scripts/package_release.sh
```

---

## 3. Generated Release Artifacts

Release packages are generated into the `dist/` directory:
- `klyrdb-0.12.0-windows-x64.zip` (Binaries, headers, CMake config, examples, documentation)
- `klyrdb-0.12.0-windows-x64.zip.sha256` (Cryptographic verification checksum)
- `klyrdb-0.12.0-linux-x64.tar.gz` (POSIX archive)
- `klyrdb-0.12.0-linux-x64.tar.gz.sha256`

---

## 4. Release Checklist

1. Verify working directory is clean.
2. Confirm all 50 CTest targets pass in Debug and Release configurations.
3. Validate standalone consumer examples compile and run from an installed prefix.
4. Execute release packaging script.
5. Verify SHA-256 checksum matches the generated archive.
