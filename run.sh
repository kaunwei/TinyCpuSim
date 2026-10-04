#!/bin/bash
# TinyCpuSim Unified Top-Level Interactive Launcher & Workflow Manager
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIGS_DIR="${PROJECT_ROOT}/configs"
FIXTURES_DIR="${PROJECT_ROOT}/tests/fixtures"

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

print_banner() {
    echo "============================================================"
    echo "       TinyCpuSim - ARM Out-of-Order CPU Simulator          "
    echo "============================================================"
}

run_interactive_exp() {
    "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" "$@"
}

show_help() {
    print_banner
    echo "TinyCpuSim Unified Command Line & Menu Interface Manual"
    echo "============================================================"
    echo "Workflow Steps:"
    echo "  ./run.sh build                      # [Step 1] Build simulator (Release mode)"
    echo "  ./run.sh test                       # [Step 2] Run 186 unit & regression tests (parallel ctest)"
    echo "  ./run.sh ubench [bpu|exec|rob|cache|all] # [Step 3] Run component microbenchmarks (<1% Δ)"
    echo "  ./run.sh sim [elf] [config]         # [Step 4] Run simulation (zero-args reads current.cfg)"
    echo "  ./run.sh gem5 [--all]               # [Step 5] Compare accuracy vs gem5 golden (100% PASS)"
    echo ""
    echo "Microarchitectural Slicing & Profiling:"
    echo "  ./run.sh slice [elf]                # Interval slice comparison vs gem5 (locate drift points)"
    echo ""
    echo "Configuration & Editing:"
    echo "  ./run.sh edit                       # Direct vi editing of active configs/current.cfg"
    echo "  ./run.sh show                       # Display full active microarchitecture dashboard"
    echo "  ./run.sh list                       # List all presets and snapshots in default/ and save/"
    echo "  ./run.sh config                     # Launch interactive configuration manager TUI"
    echo ""
    echo "Microarchitectural Experiments & Sweeps:"
    echo "  ./run.sh exp                        # Run experiment on active config (configs/current.cfg) vs baseline"
    echo "  ./run.sh exp [elf]                  # Run experiment on target ELF using active config"
    echo "  ./run.sh exp --set k=v              # Run experiment with hardware overrides (e.g. ooo=false)"
    echo "  ./run.sh sweep                      # Run dynamic parameter sweep on hardware knobs"
    echo ""
    echo "Catalogs & Utilities:"
    echo "  ./run.sh knobs                      # List all tunable hardware parameters & units"
    echo "  ./run.sh elfs                       # List all built-in benchmark ELF workloads"
    echo "  ./run.sh setup                      # Install required system & Python dependencies"
    echo "  ./run.sh clean                      # Clean build artifacts"
    echo "============================================================"
}

show_menu() {
    print_banner
    echo "Please choose a step or action:"
    echo "  [C] Config:   Configure Active Simulation, Hardware Knobs, Presets, Save/Load"
    echo "  [1] Step 1:   Build Project (Release Mode)"
    echo "  [2] Step 2:   Run Full Test Suite (186 Unit & Regression Tests)"
    echo "  [3] Step 3:   Run Component Microbenchmarks (uBench)"
    echo "  [4] Step 4:   Run CPU Simulation (reads configs/current.cfg automatically)"
    echo "  [5] Step 5:   Compare Accuracy against gem5 Golden Reference (100% PASS)"
    echo "  [S] Slice:    Run Multi-Slice Interval Profiling & Drift Point Locator"
    echo "  [6] Exp:      Run Experiment on Active Config vs Baseline"
    echo "  [7] Sweep:    Run Batch Parameter Sweep across Microarchitectural Knobs"
    echo "  [H] Help:     View Complete Command & Usage Manual"
    echo "  [0] Exit"
    echo "============================================================"
    read -r -p "Enter choice [C, 1-7, S, H, 0]: " choice
    case "${choice}" in
        c|C|config) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" ;;
        1) "${PROJECT_ROOT}/scripts/01_build.sh" ;;
        2) "${PROJECT_ROOT}/scripts/02_run_tests.sh" ;;
        3) "${PROJECT_ROOT}/scripts/03_run_ubench.sh" ;;
        4) "${PROJECT_ROOT}/scripts/04_run_simulation.sh" ;;
        5) "${PROJECT_ROOT}/scripts/05_compare_gem5.sh" --all ;;
        s|S|slice) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/compare_slices.py" --all-golden ;;
        6) run_interactive_exp ;;
        7) "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" sweep ;;
        h|H|help) show_help ;;
        0|q|Q) echo "Goodbye!"; exit 0 ;;
        *) echo "Invalid option."; exit 1 ;;
    esac
}

if [ $# -eq 0 ]; then
    show_menu
else
    COMMAND="$1"
    shift || true
    case "${COMMAND}" in
        c|cfg|config)
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
        1|build)
            "${PROJECT_ROOT}/scripts/01_build.sh" "$@"
            ;;
        2|test|tests)
            "${PROJECT_ROOT}/scripts/02_run_tests.sh" "$@"
            ;;
        3|ubench)
            "${PROJECT_ROOT}/scripts/03_run_ubench.sh" "$@"
            ;;
        4|sim|simulate)
            "${PROJECT_ROOT}/scripts/04_run_simulation.sh" "$@"
            ;;
        5|gem5)
            "${PROJECT_ROOT}/scripts/05_compare_gem5.sh" "$@"
            ;;
        slice|slices)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/compare_slices.py" "$@"
            ;;
        6|exp|experiment)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" "$@"
            ;;
        7|sweep)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/config.py" sweep "$@"
            ;;
        knobs|params|list-params)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" --list-params
            ;;
        elfs|list-elfs)
            "${PYTHON_BIN}" "${PROJECT_ROOT}/scripts/experiment.py" --list-elfs
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
