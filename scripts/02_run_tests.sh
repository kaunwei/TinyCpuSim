#!/bin/bash
# [Step 2] TinyCpuSim Regression Test Runner
# Runs the full suite of 168+ unit and microarchitectural tests.
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# Resolve default installation paths for tools, avoiding reliance on env/PATH
find_default_tool() {
    local name="$1"
    for candidate in \
        "/usr/bin/${name}" \
        "/usr/local/bin/${name}" \
        "/opt/homebrew/bin/${name}" \
        "/bin/${name}"; do
        if [ -x "${candidate}" ]; then
            echo "${candidate}"
            return 0
        fi
    done
    # Fallback only if not in default installation directories
    if command -v "${name}" >/dev/null 2>&1; then
        command -v "${name}"
        return 0
    fi
    echo "${name}"
}

CTEST_BIN=$(find_default_tool "ctest")

if [ ! -d "${BUILD_DIR}" ]; then
    echo "Build directory not found. Running Step 1 first..."
    "${PROJECT_ROOT}/scripts/01_build.sh"
fi

cd "${BUILD_DIR}"

NUM_CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
JOBS=${JOBS:-${NUM_CORES}}

EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform|-p)
            EXTRA_ARGS+=("-L" "$2")
            shift 2
            ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

echo "============================================================"
echo " [Step 2/5] Running Full Regression Suite (Parallel: ${JOBS} workers)"
echo "============================================================"
echo " CTest Path: ${CTEST_BIN}"

"${CTEST_BIN}" -j"${JOBS}" --output-on-failure "${EXTRA_ARGS[@]}"

echo ""
echo "============================================================"
echo " [SUCCESS] 100% tests passed successfully!"
echo " Next step: Run microbenchmarks with './scripts/03_run_ubench.sh'"
echo "============================================================"
