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
cp -r "${ROOT_DIR}/examples" "${STAGING_DIR}/"
find "${STAGING_DIR}/examples" -type d -name "build" -exec rm -rf {} + 2>/dev/null || true
cp -r "${ROOT_DIR}/docs" "${STAGING_DIR}/"

echo "[6/6] Generating tar.gz archive and SHA-256 checksum..."
TAR_PATH="${DIST_DIR}/${PACKAGE_NAME}.tar.gz"
tar -czf "${TAR_PATH}" -C "${DIST_DIR}" "${PACKAGE_NAME}"
sha256sum "${TAR_PATH}" > "${TAR_PATH}.sha256"

echo "Release package created: ${TAR_PATH}"
