#!/bin/bash
# TinyCpuSim Unified Top-Level Interactive Platform Launcher & Workflow Manager
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
CONFIGS_DIR="${PROJECT_ROOT}/configs"
FIXTURES_DIR="${PROJECT_ROOT}/tests/fixtures"

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

print_banner() {
    echo "============================================================"
    echo "       TinyCpuSim - Modular Virtual Platform Manager        "
    echo "============================================================"
}

show_help() {
    print_banner
    echo "TinyCpuSim Modular Platform Launcher & CLI Manual"
    echo "============================================================"
    echo "2-Step Interactive Mode:"
    echo "  ./run.sh                            # Launches Step 1 (Platform Selection)"
    echo ""
    echo "Platform Direct CLI Invocations:"
    echo "  ./run.sh perf [ubench|gem5|exp|sweep|demo|test]"
    echo "  ./run.sh mcu [pty|test|demo|fw]"
    echo "  ./run.sh accel [demo|test|cosim]"
    echo "  ./run.sh all [build|test]"
    echo ""
    echo "Global Shortcuts & Utilities:"
    echo "  ./run.sh build                      # Build all targets in parallel"
    echo "  ./run.sh test                       # Run full test suite across all 8 cores"
    echo "  ./run.sh ubench [suite]             # Microarchitectural counters vs gem5"
    echo "  ./run.sh gem5 [--all]               # gem5 golden comparison"
    echo "  ./run.sh sim [elf] [cfg]            # Run CPU simulation"
    echo "  ./run.sh exp                        # Run microarchitecture experiment"
    echo "  ./run.sh config                     # Hardware configuration manager"
    echo "  ./run.sh fw                         # Baremetal firmware runner"
    echo "  ./run.sh clean                      # Clean build directory"
    echo "  ./run.sh help                       # Display this help manual"
    echo "============================================================"
}

# Platform 1 Action Handler
handle_perf_action() {
    local action="${1:-menu}"
    if [ "${action}" = "menu" ]; then
        echo "Platform: [01_basic_perf] CPU Microarchitecture & Correlation"
        echo "  [1] uBench:  Run Component Microbenchmarks (<1% Δ vs gem5)"
        echo "  [2] gem5:    Compare Accuracy vs gem5 Golden Reference"
        echo "  [3] Exp:     Run Experiment on Active Config vs Baseline"
        echo "  [4] Sweep:   Run Hardware Parameter Sweep"
        echo "  [5] Sim:     Run CPU Simulation (configs/current.cfg)"
        echo "  [6] Demo:    Run 01_basic_perf Demo Binary"
        echo "  [7] Test:    Run Platform Perf Tests (-L perf)"
        echo "  [0] Back"
        read -r -p "Enter choice [1-7, 0]: " act_choice
        case "${act_choice}" in
            1) "${PROJECT_ROOT}/scripts/03_run_ubench.sh" ;;
            2) "${PROJECT_ROOT}/scripts/05_compare_gem5.sh" --all ;;
            3) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" ;;
            4) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" sweep ;;
            5) "${PROJECT_ROOT}/scripts/04_run_simulation.sh" ;;
            6) "${BUILD_DIR}/platforms/01_basic_perf/platform_01_perf_demo" ;;
            7) "${PROJECT_ROOT}/scripts/02_run_tests.sh" --platform perf ;;
            0) show_platform_menu ;;
            *) echo "Invalid choice."; exit 1 ;;
        esac
    else
        case "${action}" in
            ubench) "${PROJECT_ROOT}/scripts/03_run_ubench.sh" "${@:2}" ;;
            gem5) "${PROJECT_ROOT}/scripts/05_compare_gem5.sh" "${@:2}" ;;
            exp|experiment) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" "${@:2}" ;;
            sweep) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" sweep "${@:2}" ;;
            sim|simulate) "${PROJECT_ROOT}/scripts/04_run_simulation.sh" "${@:2}" ;;
            demo) "${BUILD_DIR}/platforms/01_basic_perf/platform_01_perf_demo" ;;
            test|tests) "${PROJECT_ROOT}/scripts/02_run_tests.sh" --platform perf ;;
            *) echo "Unknown perf action: ${action}"; exit 1 ;;
        esac
    fi
}

# Platform 2 Action Handler
handle_mcu_action() {
    local action="${1:-menu}"
    if [ "${action}" = "menu" ]; then
        echo "Platform: [02_basic_mcu] Cortex-M3 SoC & Baremetal MCU"
        echo "  [1] PTY:     Run Interactive UART PTY Terminal Demo"
        echo "  [2] Test:    Run Baremetal Peripheral Tests (-L mcu)"
        echo "  [3] Demo:    Run 02_basic_mcu Demo Binary"
        echo "  [4] FW:      Run Firmware Environment (PTY Terminal, Scheduler, Watchdog)"
        echo "  [0] Back"
        read -r -p "Enter choice [1-4, 0]: " act_choice
        case "${act_choice}" in
            1) "${PROJECT_ROOT}/scripts/run_firmware.sh" uart_echo --pty ;;
            2) "${PROJECT_ROOT}/scripts/02_run_tests.sh" --platform mcu ;;
            3) "${BUILD_DIR}/platforms/02_basic_mcu/platform_02_mcu_demo" ;;
            4) "${PROJECT_ROOT}/scripts/run_firmware.sh" ;;
            0) show_platform_menu ;;
            *) echo "Invalid choice."; exit 1 ;;
        esac
    else
        case "${action}" in
            pty) "${PROJECT_ROOT}/scripts/run_firmware.sh" uart_echo --pty ;;
            test|tests) "${PROJECT_ROOT}/scripts/02_run_tests.sh" --platform mcu ;;
            demo) "${BUILD_DIR}/platforms/02_basic_mcu/platform_02_mcu_demo" ;;
            fw|firmware) "${PROJECT_ROOT}/scripts/run_firmware.sh" "${@:2}" ;;
            *) echo "Unknown mcu action: ${action}"; exit 1 ;;
        esac
    fi
}

# Platform 3 Action Handler
handle_accel_action() {
    local action="${1:-menu}"
    if [ "${action}" = "menu" ]; then
        echo "Platform: [03_coprocessor_accel] Dual-Core Heterogeneous SoC & Accel"
        echo "  [1] Demo:    Run 03_coprocessor_accel Demo Binary"
        echo "  [2] Test:    Run Co-processor & Accel Platform Tests (-L accel)"
        echo "  [3] CoSim:   Run Full Dual-Core Virtual Platform Test"
        echo "  [0] Back"
        read -r -p "Enter choice [1-3, 0]: " act_choice
        case "${act_choice}" in
            1) "${BUILD_DIR}/platforms/03_coprocessor_accel/platform_03_accel_demo" ;;
            2) "${PROJECT_ROOT}/scripts/02_run_tests.sh" --platform accel ;;
            3) ctest --test-dir "${BUILD_DIR}" -R virtual_platform_test --output-on-failure ;;
            0) show_platform_menu ;;
            *) echo "Invalid choice."; exit 1 ;;
        esac
    else
        case "${action}" in
            demo) "${BUILD_DIR}/platforms/03_coprocessor_accel/platform_03_accel_demo" ;;
            test|tests) "${PROJECT_ROOT}/scripts/02_run_tests.sh" --platform accel ;;
            cosim) ctest --test-dir "${BUILD_DIR}" -R virtual_platform_test --output-on-failure ;;
            *) echo "Unknown accel action: ${action}"; exit 1 ;;
        esac
    fi
}

# All Platforms Handler
handle_all_action() {
    local action="${1:-menu}"
    if [ "${action}" = "menu" ]; then
        echo "Universal: All Platforms"
        echo "  [1] Build:   Build All Platforms & Core (Parallel: $(nproc) cores)"
        echo "  [2] Test:    Run All Tests Across All Platforms (Parallel: $(nproc) workers)"
        echo "  [3] gem5:    Run Full gem5 & Microbench Suite"
        echo "  [0] Back"
        read -r -p "Enter choice [1-3, 0]: " act_choice
        case "${act_choice}" in
            1) "${PROJECT_ROOT}/scripts/01_build.sh" ;;
            2) "${PROJECT_ROOT}/scripts/02_run_tests.sh" ;;
            3) "${PROJECT_ROOT}/scripts/05_compare_gem5.sh" --all ;;
            0) show_platform_menu ;;
            *) echo "Invalid choice."; exit 1 ;;
        esac
    else
        case "${action}" in
            build) "${PROJECT_ROOT}/scripts/01_build.sh" "${@:2}" ;;
            test|tests) "${PROJECT_ROOT}/scripts/02_run_tests.sh" "${@:2}" ;;
            gem5) "${PROJECT_ROOT}/scripts/05_compare_gem5.sh" --all ;;
            *) echo "Unknown all action: ${action}"; exit 1 ;;
        esac
    fi
}

show_platform_menu() {
    print_banner
    echo "Step 1: Select Virtual Platform / Project"
    echo "============================================================"
    echo "  [1] 01_basic_perf:        Microarchitecture & gem5 Golden Calibration"
    echo "  [2] 02_basic_mcu:         Cortex-M3 SoC & Baremetal Firmware (PTY, UART, DMA)"
    echo "  [3] 03_coprocessor_accel: Dual-Core SoC, Mailbox & 2D Conv Acceleration"
    echo "  [A] All Platforms:        Universal Parallel Build & Full Regression Suite"
    echo ""
    echo "Global Utilities & Shortcuts:"
    echo "  [C] Config:   Hardware Configuration Manager"
    echo "  [L] Clean:    Clean Build Artifacts"
    echo "  [H] Help:     View Command Manual"
    echo "  [0] Exit"
    echo "============================================================"
    read -r -p "Enter choice [1-3, A, C, L, H, 0]: " choice
    case "${choice}" in
        1) handle_perf_action "menu" ;;
        2) handle_mcu_action "menu" ;;
        3) handle_accel_action "menu" ;;
        a|A|all) handle_all_action "menu" ;;
        c|C|config) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" ;;
        l|L|clean) rm -rf "${BUILD_DIR}"; echo "[OK] Cleaned build directory." ;;
        h|H|help) show_help ;;
        0|q|Q) echo "Goodbye!"; exit 0 ;;
        *) echo "Invalid option."; exit 1 ;;
    esac
}

if [ $# -eq 0 ]; then
    show_platform_menu
else
    COMMAND="$1"
    shift || true
    case "${COMMAND}" in
        1|perf|01_basic_perf)
            handle_perf_action "$@"
            ;;
        2|mcu|02_basic_mcu)
            handle_mcu_action "$@"
            ;;
        3|accel|03_coprocessor_accel)
            handle_accel_action "$@"
            ;;
        a|all)
            handle_all_action "$@"
            ;;
        build)
            "${PROJECT_ROOT}/scripts/01_build.sh" "$@"
            ;;
        test|tests)
            "${PROJECT_ROOT}/scripts/02_run_tests.sh" "$@"
            ;;
        ubench)
            "${PROJECT_ROOT}/scripts/03_run_ubench.sh" "$@"
            ;;
        sim|simulate)
            "${PROJECT_ROOT}/scripts/04_run_simulation.sh" "$@"
            ;;
        gem5)
            "${PROJECT_ROOT}/scripts/05_compare_gem5.sh" "$@"
            ;;
        exp|experiment)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" "$@"
            ;;
        cfg|config)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" "$@"
            ;;
        edit|vi)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" edit
            ;;
        show|view)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" show
            ;;
        list|ls)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" list
            ;;
        sweep)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" sweep "$@"
            ;;
        knobs|params|list-params)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" --list-params
            ;;
        elfs|list-elfs)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" --list-elfs
            ;;
        fw|firmware)
            "${PROJECT_ROOT}/scripts/run_firmware.sh" "$@"
            ;;
        setup|deps|install-deps)
            "${PROJECT_ROOT}/scripts/install_deps.sh" "$@"
            ;;
        clean)
            echo "Cleaning build directory..."
            rm -rf "${PROJECT_ROOT}/build"
            echo "[OK] Cleaned."
            ;;
        -h|--help|help)
            show_help
            ;;
        *)
            echo "Unknown command: ${COMMAND}"
            echo "Run './run.sh --help' for available commands."
            exit 1
            ;;
    esac
fi
