#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# SQLite Fixed-Schema Storage (FSS) Build & Benchmark Script
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQLITE_DIR="${SCRIPT_DIR}/sqlite"
TARGET_COMMIT="d54522a5b832422ab893a1e9fa7a99ab314ea4e2"
SQLITE_REPO_URL="https://github.com/sqlite/sqlite.git"

echo "======================================================================="
echo " SQLite Fixed-Schema Storage (FSS) Automated Build & Benchmark"
echo " Target SQLite Commit: ${TARGET_COMMIT}"
echo "======================================================================="

# ------------------------------------------------------------------------------
# 1. Fetch SQLite Sources
# ------------------------------------------------------------------------------
if [ ! -d "${SQLITE_DIR}/.git" ]; then
    echo "[1/5] Cloning SQLite repository..."
    git clone "${SQLITE_REPO_URL}" "${SQLITE_DIR}"
    cd "${SQLITE_DIR}"
    git checkout "${TARGET_COMMIT}"
    cd "${SCRIPT_DIR}"
else
    echo "[1/5] Preparing SQLite repository..."
    cd "${SQLITE_DIR}"
    git reset --hard
    git clean -fd
    git checkout "${TARGET_COMMIT}"
    cd "${SCRIPT_DIR}"
fi

# ------------------------------------------------------------------------------
# 2. Apply FSS Changes to SQLite
# ------------------------------------------------------------------------------
echo "[2/5] Applying FSS patches to SQLite..."
cp "${SCRIPT_DIR}/src/fss.h" "${SQLITE_DIR}/src/"
cp "${SCRIPT_DIR}/src/fss.c" "${SQLITE_DIR}/src/"

cd "${SQLITE_DIR}"
git apply "${SCRIPT_DIR}/patches/sqlite-fss.patch"

echo "  -> Regenerating SQLite pragma table (pragma.h)..."
tclsh tool/mkpragmatab.tcl
cd "${SCRIPT_DIR}"

# ------------------------------------------------------------------------------
# 3. Build SQLite
# ------------------------------------------------------------------------------
echo "[3/5] Building SQLite amalgamation and binaries..."
cd "${SQLITE_DIR}"
if [ ! -f "Makefile" ]; then
    ./configure --dev
fi
make -j"$(nproc)" sqlite3 sqlite3.c
gcc -O2 -c sqlite3.c -o sqlite3.o
cd "${SCRIPT_DIR}"

# ------------------------------------------------------------------------------
# 4. Build Test Suites and Benchmark
# ------------------------------------------------------------------------------
echo "[4/5] Building test suites and benchmark..."
mkdir -p "${SCRIPT_DIR}/bin"

# 4a. Standalone FSS format unit tests
gcc -O2 -I"${SCRIPT_DIR}/src" \
    "${SCRIPT_DIR}/test/test_fss.c" "${SCRIPT_DIR}/src/fss.c" \
    -o "${SCRIPT_DIR}/bin/test_fss"

# 4b. PRAGMA syntax and gating test suite
gcc -O2 -I"${SCRIPT_DIR}/src" -I"${SQLITE_DIR}" \
    "${SCRIPT_DIR}/test/test_fss_pragma.c" "${SCRIPT_DIR}/src/fss.c" "${SQLITE_DIR}/sqlite3.o" \
    -lpthread -ldl -lm \
    -o "${SCRIPT_DIR}/bin/test_fss_pragma"

# 4c. 1,000,000 records benchmark (INSERT, SELECT, UPDATE, Disk Space comparison)
gcc -O2 -I"${SCRIPT_DIR}/src" -I"${SQLITE_DIR}" \
    "${SCRIPT_DIR}/benchmark/benchmark.c" "${SCRIPT_DIR}/src/fss.c" "${SQLITE_DIR}/sqlite3.o" \
    -lpthread -ldl -lm \
    -o "${SCRIPT_DIR}/bin/benchmark"

echo "  -> Built bin/test_fss, bin/test_fss_pragma, bin/benchmark"

# ------------------------------------------------------------------------------
# 5. Run Verification & Benchmark
# ------------------------------------------------------------------------------
echo "[5/5] Running test suites and benchmark..."

echo "--- [A] Running FSS Unit Tests ---"
"${SCRIPT_DIR}/bin/test_fss"

echo ""
echo "--- [B] Running PRAGMA Tests ---"
"${SCRIPT_DIR}/bin/test_fss_pragma"

echo ""
echo "--- [C] Running 1,000,000 Operations Benchmark ---"
"${SCRIPT_DIR}/bin/benchmark"

echo ""
echo "======================================================================="
echo " Build and Benchmark Completed Successfully!"
echo "======================================================================="
