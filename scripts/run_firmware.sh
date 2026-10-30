#!/bin/bash
# TinyCpuSim Firmware Environment Launcher & Project Manager
set -eo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
SIM_BIN="${BUILD_DIR}/tinyarmsim"
PROJECTS_DIR="${PROJECT_ROOT}/firmware/projects"
PYTHON_BIN="/usr/bin/python3"
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
    if [ -t 0 ]; then
        stty sane 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

show_fw_help() {
    echo "============================================================"
    echo "TinyCpuSim Firmware Environment Launcher"
    echo "============================================================"
    echo "Usage: ./run.sh fw [COMMAND|PROJECT_NAME|OPTIONS] [ELF_PATH]"
    echo ""
    echo "Commands:"
    echo "  build                  Build baremetal firmware ELFs"
    echo "  test [ELF]             Run bounded automated acceptance test with watchdog"
    echo "  interactive [ELF]      Run firmware interactively in terminal"
    echo "  list                   List available firmware projects"
    echo ""
    echo "Projects:"
    echo "  uart_echo              Interactive 3-round UART echo demo"
    echo "  scheduler              Preemptive task scheduler acceptance app"
    echo "  shell                  Interactive firmware command shell"
    echo "  reference              Reference application (timer, scheduler, shell)"
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
    echo "  ./run.sh fw"
    echo "  ./run.sh fw uart_echo"
    echo "  ./run.sh fw uart_echo --pty"
    echo "  ./run.sh fw --debug --pty"
    echo "============================================================"
}

build_builtins() {
    echo "[run.sh fw] Building built-in firmware ELFs..."
    ninja -C "${BUILD_DIR}" baremetal_firmware_elf scheduler_acceptance_elf shell_acceptance_elf reference_app_elf
}

build_all_projects() {
    build_builtins
    if [ -d "${PROJECTS_DIR}" ]; then
        for proj in "${PROJECTS_DIR}"/*; do
            if [ -d "${proj}" ] && [ -f "${proj}/Makefile" ]; then
                echo "[run.sh fw] Building project: $(basename "${proj}")..."
                make -C "${proj}"
            fi
        done
    fi
    echo "[run.sh fw] All firmware builds complete."
}

list_projects() {
    echo "============================================================"
    echo "Available Firmware Projects:"
    echo "============================================================"
    local idx=1
    if [ -d "${PROJECTS_DIR}" ]; then
        for proj in "${PROJECTS_DIR}"/*; do
            if [ -d "${proj}" ]; then
                local name
                name="$(basename "${proj}")"
                local desc="Custom project"
                if [ -f "${proj}/banner.txt" ]; then
                    desc="$(head -n 2 "${proj}/banner.txt" | tail -n 1 | sed 's/^[[:space:]]*//')"
                fi
                echo "  [${idx}] ${name} (${proj}) - ${desc}"
                idx=$((idx + 1))
            fi
        done
    fi
    echo "  [${idx}] scheduler_acceptance (firmware/apps/scheduler_acceptance.c)"
    idx=$((idx + 1))
    echo "  [${idx}] shell_acceptance (firmware/apps/shell_acceptance.c)"
    idx=$((idx + 1))
    echo "  [${idx}] reference_app (firmware/apps/reference_app.c)"
    echo "============================================================"
}

# Parse subcommands and options
ELF_PATH=""
PROJECT_NAME=""
BANNER_PATH=""
USE_PTY=0
USE_DEBUG=0
QUIET_MODE=0
MAX_STEPS=""
IS_TEST=0
IS_BUILD=0
FORCE_INTERACTIVE=0

while [ $# -gt 0 ]; do
    case "$1" in
        list)
            list_projects
            exit 0
            ;;
        build)
            IS_BUILD=1
            shift
            if [ $# -gt 0 ] && [[ "$1" != -* ]]; then
                PROJECT_NAME="$1"
                shift
            fi
            ;;
        test)
            IS_TEST=1
            shift
            ;;
        interactive)
            FORCE_INTERACTIVE=1
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
            if [ -z "${PROJECT_NAME}" ] && [ -z "${ELF_PATH}" ]; then
                if [ -f "$1" ]; then
                    ELF_PATH="$1"
                else
                    PROJECT_NAME="$1"
                fi
            fi
            shift
            ;;
    esac
done

# If build mode requested
if [ "${IS_BUILD}" -eq 1 ]; then
    if [ -n "${PROJECT_NAME}" ]; then
        if [ -d "${PROJECTS_DIR}/${PROJECT_NAME}" ] && [ -f "${PROJECTS_DIR}/${PROJECT_NAME}/Makefile" ]; then
            echo "[run.sh fw] Building project: ${PROJECT_NAME}..."
            make -C "${PROJECTS_DIR}/${PROJECT_NAME}"
            exit 0
        fi
    fi
    build_all_projects
    exit 0
fi

# Ensure simulator binary exists
if [ ! -f "${SIM_BIN}" ]; then
    echo "[run.sh fw] Simulator binary '${SIM_BIN}' not found. Building..."
    ninja -C "${BUILD_DIR}" tinyarmsim
fi

# Interactive project selection menu if no project or ELF is given
if [ -z "${PROJECT_NAME}" ] && [ -z "${ELF_PATH}" ]; then
    if [ "${IS_TEST}" -eq 1 ] || [ "${USE_PTY}" -eq 1 ] || [ ! -t 0 ]; then
        PROJECT_NAME="scheduler_acceptance"
    else
        echo "============================================================"
        echo "        TinyCpuSim Firmware Project Selector                "
        echo "============================================================"
        echo "Available Firmware Projects:"
        echo "  [1] uart_echo            (Interactive 3-round UART echo demo)"
        echo "  [2] scheduler_acceptance (Preemptive task scheduler)"
        echo "  [3] shell_acceptance     (Interactive firmware command shell)"
        echo "  [4] reference_app        (Reference app with timer & peripherals)"
        echo "============================================================"
        read -r -p "Select project [1-4, default: 1]: " proj_choice
        case "${proj_choice}" in
            1|""|echo|uart_echo)
                PROJECT_NAME="uart_echo"
                ;;
            2|scheduler|sched)
                PROJECT_NAME="scheduler_acceptance"
                ;;
            3|shell)
                PROJECT_NAME="shell_acceptance"
                ;;
            4|ref|reference)
                PROJECT_NAME="reference_app"
                ;;
            *)
                PROJECT_NAME="${proj_choice}"
                ;;
        esac
    fi
fi

# Resolve project name to ELF path and banner
if [ -n "${PROJECT_NAME}" ] && [ -z "${ELF_PATH}" ]; then
    case "${PROJECT_NAME}" in
        uart_echo|echo)
            PROJ_DIR="${PROJECTS_DIR}/uart_echo"
            ELF_PATH="${PROJ_DIR}/uart_echo.elf"
            if [ -f "${PROJ_DIR}/banner.txt" ]; then
                BANNER_PATH="${PROJ_DIR}/banner.txt"
            fi
            if [ ! -f "${ELF_PATH}" ]; then
                echo "[run.sh fw] Building ${PROJECT_NAME}..."
                make -C "${PROJ_DIR}"
            fi
            ;;
        scheduler|scheduler_acceptance)
            if [ "${USE_DEBUG}" -eq 1 ]; then
                ELF_PATH="${BUILD_DIR}/scheduler_acceptance_debug.elf"
            else
                ELF_PATH="${BUILD_DIR}/scheduler_acceptance.elf"
            fi
            if [ ! -f "${ELF_PATH}" ]; then
                ninja -C "${BUILD_DIR}" scheduler_acceptance_elf
            fi
            ;;
        shell|shell_acceptance)
            ELF_PATH="${BUILD_DIR}/shell_acceptance.elf"
            if [ ! -f "${ELF_PATH}" ]; then
                ninja -C "${BUILD_DIR}" shell_acceptance_elf
            fi
            ;;
        reference|reference_app)
            if [ "${USE_DEBUG}" -eq 1 ]; then
                ELF_PATH="${BUILD_DIR}/reference_app_debug.elf"
            else
                ELF_PATH="${BUILD_DIR}/reference_app.elf"
            fi
            if [ ! -f "${ELF_PATH}" ]; then
                ninja -C "${BUILD_DIR}" reference_app_elf
            fi
            ;;
        *)
            # Check if it exists under firmware/projects/
            if [ -d "${PROJECTS_DIR}/${PROJECT_NAME}" ]; then
                PROJ_DIR="${PROJECTS_DIR}/${PROJECT_NAME}"
                ELF_PATH="${PROJ_DIR}/${PROJECT_NAME}.elf"
                if [ -f "${PROJ_DIR}/banner.txt" ]; then
                    BANNER_PATH="${PROJ_DIR}/banner.txt"
                fi
                if [ ! -f "${ELF_PATH}" ] && [ -f "${PROJ_DIR}/Makefile" ]; then
                    echo "[run.sh fw] Building ${PROJECT_NAME}..."
                    make -C "${PROJ_DIR}"
                fi
            else
                echo "Error: Unknown project '${PROJECT_NAME}'" >&2
                exit 1
            fi
            ;;
    esac
fi

# Fallback default
if [ -z "${ELF_PATH}" ]; then
    ELF_PATH="${DEFAULT_ELF}"
fi

# If test mode, run via python watchdog harness
if [ "${IS_TEST}" -eq 1 ]; then
    "${PYTHON_BIN}" -c "
import sys
sys.path.insert(0, '${PROJECT_ROOT}')
from scripts.firmware_test_harness import run_service_watchdog
try:
    res = run_service_watchdog(
        '${SIM_BIN}',
        '${ELF_PATH}',
        target_service_predicate=lambda out: 'PASS' in out or 'Echo' in out,
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

# If direct interactive mode requested or running uart_echo without --pty
if [ "${USE_PTY}" -eq 0 ] && ([ "${PROJECT_NAME}" = "uart_echo" ] || [ "${FORCE_INTERACTIVE}" -eq 1 ]); then
    TERMINAL_BRIDGE="${PROJECT_ROOT}/scripts/uart_terminal.py"
    BRIDGE_ARGS=("${SIM_BIN}" "${ELF_PATH}")
    if [ -n "${BANNER_PATH}" ]; then
        BRIDGE_ARGS+=("${BANNER_PATH}")
    else
        BRIDGE_ARGS+=("")
    fi
    if [ -n "${MAX_STEPS}" ]; then
        BRIDGE_ARGS+=("--max-steps" "${MAX_STEPS}")
    fi
    exec "${PYTHON_BIN}" "${TERMINAL_BRIDGE}" "${BRIDGE_ARGS[@]}"
fi

# Default/PTY simulator execution
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

if [ -n "${BANNER_PATH}" ] && [ -f "${BANNER_PATH}" ]; then
    cat "${BANNER_PATH}"
    echo "------------------------------------------------------------"
fi

"${SIM_BIN}" "${SIM_ARGS[@]}" &
CHILD_PID=$!

wait "${CHILD_PID}"
EXIT_CODE=$?
CHILD_PID=""
exit "${EXIT_CODE}"
