#!/bin/bash
# [Step 5] TinyCpuSim vs gem5 Golden Reference Accuracy Comparator
# Runs regression accuracy comparison against gem5 cycle-accurate golden models.
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Resolve default installation path for Python, avoiding env/PATH where possible
find_default_python() {
    for candidate in \
        "/usr/bin/python3" \
        "/usr/local/bin/python3" \
        "/opt/homebrew/bin/python3" \
        "/usr/bin/python"; do
        if [ -x "${candidate}" ]; then
            echo "${candidate}"
            return 0
        fi
    done
    if command -v python3 >/dev/null 2>&1; then
        command -v python3
        return 0
    fi
    echo "python3"
}

PYTHON_BIN=$(find_default_python)

echo "============================================================"
echo " [Step 5/5] TinyCpuSim vs gem5 Golden Reference Accuracy   "
echo "============================================================"

"${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/verify_gem5.py" "$@"
