#!/bin/bash
# [Step 1] TinyCpuSim Build Script
# Automatically detects system cores, configures CMake Release mode, and builds all binaries.
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

CMAKE_BIN=$(find_default_tool "cmake")
NINJA_BIN=$(find_default_tool "ninja")
CXX_BIN=$(find_default_tool "g++")
if [ ! -x "${CXX_BIN}" ]; then
    CXX_BIN=$(find_default_tool "clang++")
fi
CC_BIN=$(find_default_tool "gcc")
if [ ! -x "${CC_BIN}" ]; then
    CC_BIN=$(find_default_tool "clang")
fi

GENERATOR_ARGS=()
if [ -x "${NINJA_BIN}" ]; then
    GENERATOR_ARGS=("-GNinja" "-DCMAKE_MAKE_PROGRAM=${NINJA_BIN}")
fi

echo "============================================================"
echo " [Step 1/5] Building TinyCpuSim (Release Mode)              "
echo "============================================================"
echo " CMake Path:    ${CMAKE_BIN}"
echo " C++ Compiler:  ${CXX_BIN}"
echo " C Compiler:    ${CC_BIN}"
[ -x "${NINJA_BIN}" ] && echo " Build Tool:    ${NINJA_BIN}"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Check if existing CMakeCache.txt has stale non-existent compiler or generator mismatches
if [ -f "CMakeCache.txt" ]; then
    CACHED_CXX=$(grep "^CMAKE_CXX_COMPILER:" CMakeCache.txt | cut -d'=' -f2 || true)
    CACHED_GEN=$(grep "^CMAKE_GENERATOR:" CMakeCache.txt | cut -d'=' -f2 || true)
    NEED_CLEAN=0
    if [ -n "${CACHED_CXX}" ] && [ ! -x "${CACHED_CXX}" ]; then
        NEED_CLEAN=1
    fi
    if [ -x "${NINJA_BIN}" ] && [ "${CACHED_GEN}" = "Unix Makefiles" ]; then
        NEED_CLEAN=1
    fi
    if [ ! -x "${NINJA_BIN}" ] && [ "${CACHED_GEN}" = "Ninja" ]; then
        NEED_CLEAN=1
    fi
    if [ "${NEED_CLEAN}" -eq 1 ]; then
        echo "[INFO] Cleaning stale or mismatched CMake cache..."
        rm -rf CMakeCache.txt CMakeFiles/ _deps/
    fi
fi

NUM_CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
JOBS=${JOBS:-${NUM_CORES}}
echo "Configuring CMake (C++17, Release, Parallelism: ${JOBS} cores)..."

"${CMAKE_BIN}" "${GENERATOR_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX_BIN}" -DCMAKE_C_COMPILER="${CC_BIN}" ..
"${CMAKE_BIN}" --build . -j"${JOBS}"

echo ""
echo "============================================================"
echo " [SUCCESS] Build completed!"
echo " Primary simulator binary: ${BUILD_DIR}/tinycpusim"
echo " Next step: Run unit tests using './scripts/02_run_tests.sh'"
echo "============================================================"
