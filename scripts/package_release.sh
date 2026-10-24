#!/usr/bin/env bash
set -euo pipefail

VERSION="0.12.0"
BUILD_TYPE="Release"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build_release"
DIST_DIR="${ROOT_DIR}/dist"
PACKAGE_NAME="klyrdb-${VERSION}-linux-x64"
STAGING_DIR="${DIST_DIR}/${PACKAGE_NAME}"

echo "=== Packaging KlyrDB Release v${VERSION} (${BUILD_TYPE}) ==="

mkdir -p "${STAGING_DIR}" "${DIST_DIR}"
rm -rf "${STAGING_DIR:?}"/*

echo "[1/6] Configuring CMake..."
cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DBUILD_TESTING=ON

echo "[2/6] Building targets..."
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j"$(nproc)"

echo "[3/6] Running test suite..."
ctest --test-dir "${BUILD_DIR}" -C "${BUILD_TYPE}" --output-on-failure

echo "[4/6] Installing to staging prefix..."
cmake --install "${BUILD_DIR}" --config "${BUILD_TYPE}" --prefix "${STAGING_DIR}"

echo "[5/6] Copying examples and documentation..."
cp "${ROOT_DIR}/README.md" "${STAGING_DIR}/"
if [ -f "${ROOT_DIR}/LICENSE" ]; then
    cp "${ROOT_DIR}/LICENSE" "${STAGING_DIR}/"
fi
cp -r "${ROOT_DIR}/examples" "${STAGING_DIR}/"

# Purge any build directories, compiler intermediates, CMake caches, and temporary databases
find "${STAGING_DIR}/examples" -type d \( -name "build" -o -name "build_*" -o -name ".vs" -o -name "x64" -o -name "Debug" -o -name "Release" -o -name "out" -o -name "bin" -o -name "lib" -o -name ".cache" \) -exec rm -rf {} + 2>/dev/null || true
find "${STAGING_DIR}/examples" -type f \( -name "*.obj" -o -name "*.ilk" -o -name "*.pdb" -o -name "*.tlog" -o -name "*.idb" -o -name "*.lastbuildstate" -o -name "*.vcxproj*" -o -name "*.sln" -o -name "*.slnx" -o -name "*.user" -o -name "*.suo" -o -name "CMakeCache.txt" -o -name "cmake_install.cmake" -o -name "CTestTestfile.cmake" -o -name "*.kdb" -o -name "*.wal" -o -name "*.lock" -o -name "*.tmp" -o -name "*.bak" \) -exec rm -f {} + 2>/dev/null || true

cp -r "${ROOT_DIR}/docs" "${STAGING_DIR}/"

echo "[6/6] Generating tar.gz archive and SHA-256 checksum..."
TAR_PATH="${DIST_DIR}/${PACKAGE_NAME}.tar.gz"
tar -czf "${TAR_PATH}" -C "${DIST_DIR}" "${PACKAGE_NAME}"
sha256sum "${TAR_PATH}" > "${TAR_PATH}.sha256"

echo "Release package created: ${TAR_PATH}"
