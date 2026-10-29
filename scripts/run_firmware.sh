#!/bin/bash
# TinyCpuSim Firmware Environment Launcher (Slices 18 & 19, Parent Issue 08)
set -eo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
SIM_BIN="${BUILD_DIR}/tinyarmsim"
DEFAULT_ELF="${BUILD_DIR}/scheduler_acceptance.elf"
DEFAULT_DEBUG_ELF="${BUILD_DIR}/scheduler_acceptance_debug.elf"

# Cleanup trap variables
CHILD_PID=""
PTY_FILE=""

cleanup() {
    trap - EXIT INT TERM
    if [ -n "${CHILD_PID}" ] && kill -0 "${CHILD_PID}" 2>/dev/null; then
        kill -TERM "${CHILD_PID}" 2>/dev/null || true
        wait "${CHILD_PID}" 2>/dev/null || true
    fi
    if [ -n "${PTY_FILE}" ] && [ -f "${PTY_FILE}" ]; then
        rm -f "${PTY_FILE}" 2>/dev/null || true
    fi
    # Restore terminal sanity if modified
    if [ -t 0 ]; then
        stty sane 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

show_fw_help() {
    echo "============================================================"
    echo "TinyCpuSim Firmware Environment Launcher"
    echo "============================================================"
    echo "Usage: ./run.sh fw [COMMAND|OPTIONS] [ELF_PATH]"
    echo ""
    echo "Commands:"
    echo "  build                  Build baremetal firmware ELFs"
    echo "  test [ELF]             Run bounded automated acceptance test with watchdog"
    echo "  interactive [ELF]      Run firmware interactively in terminal"
    echo ""
    echo "Options:"
    echo "  --pty                  Launch with host PTY device (prints slave path)"
    echo "  --debug                Use Debug ELF with scheduler diagnostics"
    echo "  --quiet                Suppress simulator banner; stream only UART data"
    echo "  --max-steps <N>        Bounded simulated instruction limit (default: 1B)"
    echo "  -h, --help             Display this help message"
    echo ""
    echo "Examples:"
    echo "  ./run.sh fw build"
    echo "  ./run.sh fw test"
    echo "  ./run.sh fw --pty"
    echo "  ./run.sh fw --debug --pty"
    echo "============================================================"
}

build_firmware() {
    echo "[run.sh fw] Building firmware ELFs..."
    ninja -C "${BUILD_DIR}" baremetal_firmware_elf scheduler_acceptance_elf
    echo "[run.sh fw] Firmware build complete."
}

# Parse subcommands and options
ELF_PATH=""
USE_PTY=0
USE_DEBUG=0
QUIET_MODE=0
MAX_STEPS=""
IS_TEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        build)
            build_firmware
            exit 0
            ;;
        test)
            IS_TEST=1
            shift
            ;;
        interactive)
            shift
            ;;
        --pty)
            USE_PTY=1
            shift
            ;;
        --debug)
            USE_DEBUG=1
            shift
            ;;
        --quiet)
            QUIET_MODE=1
            shift
            ;;
        --max-steps)
            shift
            MAX_STEPS="$1"
            shift
            ;;
        -h|--help|help)
            show_fw_help
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            show_fw_help
            exit 1
            ;;
        *)
            if [ -z "${ELF_PATH}" ]; then
                ELF_PATH="$1"
            fi
            shift
            ;;
    esac
done

# Resolve ELF path
if [ -z "${ELF_PATH}" ]; then
    if [ "${USE_DEBUG}" -eq 1 ]; then
        ELF_PATH="${DEFAULT_DEBUG_ELF}"
    else
        ELF_PATH="${DEFAULT_ELF}"
    fi
fi

if [ ! -f "${ELF_PATH}" ]; then
    echo "[run.sh fw] ELF binary '${ELF_PATH}' not found. Building..."
    build_firmware
fi

if [ ! -f "${SIM_BIN}" ]; then
    echo "[run.sh fw] Simulator binary '${SIM_BIN}' not found. Building..."
    ninja -C "${BUILD_DIR}" tinyarmsim
fi

# If test mode, run via python watchdog harness
if [ "${IS_TEST}" -eq 1 ]; then
    /usr/bin/python3 -c "
import sys
sys.path.insert(0, '${PROJECT_ROOT}')
from scripts.firmware_test_harness import run_service_watchdog
try:
    res = run_service_watchdog(
        '${SIM_BIN}',
        '${ELF_PATH}',
        target_service_predicate=lambda out: 'SCHEDULER_ACCEPTANCE_PASS' in out,
        step_budget=500000 if not '${MAX_STEPS}' else int('${MAX_STEPS}'),
        timeout_sec=5.0,
    )
    if res.stdout:
        print(res.stdout, end='')
    sys.exit(res.returncode)
except Exception as e:
    print(f'Test failed: {e}', file=sys.stderr)
    sys.exit(1)
"
    exit $?
fi

# Construct simulator command
SIM_ARGS=("--firmware")
if [ "${QUIET_MODE}" -eq 1 ]; then
    SIM_ARGS+=("--quiet")
fi
if [ -n "${MAX_STEPS}" ]; then
    SIM_ARGS+=("--max-steps" "${MAX_STEPS}")
fi

if [ "${USE_PTY}" -eq 1 ]; then
    PTY_DIR="${PROJECT_ROOT}/nightshift/.scratch"
    mkdir -p "${PTY_DIR}"
    PTY_FILE="${PTY_DIR}/fw_pty_$$.txt"
    SIM_ARGS+=("--uart-pty" "--pty-file" "${PTY_FILE}")
fi

SIM_ARGS+=("${ELF_PATH}")

"${SIM_BIN}" "${SIM_ARGS[@]}" &
CHILD_PID=$!

wait "${CHILD_PID}"
EXIT_CODE=$?
CHILD_PID=""
exit "${EXIT_CODE}"
