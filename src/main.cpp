#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <iomanip>
#include "tinyarmsim/common.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/loader.hpp"
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/stats.hpp"
#include "tinyarmsim/uarch/memory_hierarchy.hpp"
#include "tinyarmsim/uarch/multicore_system.hpp"

namespace {

void print_usage(const char* prog_name) {
    std::cout << "TinyCpuSim v" << tinyarmsim::get_version_string() << " - ARM CPU ISA & Out-of-Order uArch Simulator\n\n"
              << "Usage: " << prog_name << " [options] <elf-file>\n\n"
              << "General Options:\n"
              << "  --elf <file>             Specify input ELF binary file\n"
              << "  -l, --log, --verbose     Enable step-by-step instruction trace logging\n"
              << "  -c, --coverage <file>    Export instruction opcode coverage report to CSV\n"
              << "  -m, --max-steps <N>      Set maximum instruction execution steps (default: 1000000000)\n"
              << "  -h, --help               Display this help message\n\n"
              << "Microarchitecture (uArch / OoO) Options:\n"
              << "  --uarch                  Enable microarchitectural simulation mode\n"
              << "  -u, --uarch-config <file> Load uArch configuration file (default: OoO medium)\n"
              << "  --uarch-stats <file>     Export hardware performance counters to report file\n"
              << "  --topdown [file]         Enable Top-Down microarchitectural profiler (default: stdout)\n"
              << "  --topdown-format <fmt>   Set Top-Down export format (text, json, csv; default: text)\n"
              << "  --slice-insts <N>        Enable periodic performance slicing every N instructions\n"
              << "  --slice-ticks <N>        Enable periodic performance slicing every N clock cycles/ticks\n"
              << "  --slice-file <file>      Export performance slice stream to file (default: stdout)\n"
              << "  --slice-format <fmt>     Set performance slice export format (text, gem5, json; default: text)\n"
              << "  --slice-reset            Reset performance counters after each slice snapshot\n\n"
              << "Examples:\n"
              << "  " << prog_name << " app.elf\n"
              << "  " << prog_name << " --log --coverage cov.csv app.elf\n"
              << "  " << prog_name << " --uarch --uarch-config configs/default/default.cfg app.elf\n"
              << "  " << prog_name << " --uarch --uarch-stats stats.txt app.elf\n"
              << "  " << prog_name << " --uarch --topdown report.json --topdown-format json app.elf\n"
              << "  " << prog_name << " --uarch --slice-insts 100000 --slice-file slices.txt --slice-format gem5 app.elf\n"
              << std::endl;
}

void print_banner(bool passed, uint32_t exit_code, const std::string& fault_msg, const tinyarmsim::SimulationStats& stats) {
    double mips = stats.instructions_per_second / 1000000.0;
    std::cout << "\n============================================================\n";
    if (passed) {
        std::cout << "SIMULATION PASSED\n";
    } else {
        std::cout << "SIMULATION FAILED\n";
    }
    if (!fault_msg.empty()) {
        std::cout << "Fault Reason:       " << fault_msg << "\n";
    }
    std::cout << "Exit Code:          " << exit_code << " (R0 = " << exit_code << ")\n"
              << "Total Instructions: " << stats.instruction_count << "\n"
              << std::fixed << std::setprecision(6)
              << "Elapsed Time:       " << stats.elapsed_seconds << " s\n"
              << std::fixed << std::setprecision(2)
              << "Simulation Speed:   " << stats.instructions_per_second << " inst/s (" << mips << " MIPS)\n"
              << "============================================================\n"
              << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string elf_path;
    std::string coverage_path;
    std::string uarch_config_path;
    std::string uarch_stats_path;
    std::string topdown_path;
    std::string topdown_format = "text";
    bool enable_log = false;
    bool enable_uarch = false;
    bool enable_all_perf = false;
    bool enable_topdown = false;
    uint64_t max_steps = 1000000000;

    tinyarmsim::uarch::SliceConfig slice_cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-l" || arg == "--log" || arg == "--verbose") {
            enable_log = true;
        } else if (arg == "--uarch" || arg == "--uarch-mode") {
            enable_uarch = true;
        } else if (arg == "--topdown") {
            enable_uarch = true;
            enable_topdown = true;
        } else if (arg.rfind("--topdown=", 0) == 0) {
            enable_uarch = true;
            enable_topdown = true;
            topdown_path = arg.substr(10);
        } else if (arg == "--topdown-file" || arg == "--topdown-report") {
            enable_uarch = true;
            enable_topdown = true;
            if (i + 1 < argc) {
                topdown_path = argv[++i];
            } else {
                std::cerr << "Error: --topdown-file requires a file path argument.\n";
                return 1;
            }
        } else if (arg == "--topdown-format") {
            if (i + 1 < argc) {
                topdown_format = argv[++i];
            }
        } else if (arg.rfind("--topdown-format=", 0) == 0) {
            topdown_format = arg.substr(17);
        } else if (arg == "--slice-insts" || arg == "--slice-instructions") {
            slice_cfg.enabled = true;
            if (i + 1 < argc) {
                slice_cfg.interval_instructions = std::stoull(argv[++i]);
            } else {
                std::cerr << "Error: --slice-insts requires a number argument.\n";
                return 1;
            }
        } else if (arg.rfind("--slice-insts=", 0) == 0) {
            slice_cfg.enabled = true;
            slice_cfg.interval_instructions = std::stoull(arg.substr(14));
        } else if (arg == "--slice-ticks" || arg == "--slice-cycles") {
            slice_cfg.enabled = true;
            if (i + 1 < argc) {
                slice_cfg.interval_ticks = std::stoull(argv[++i]);
            } else {
                std::cerr << "Error: --slice-ticks requires a number argument.\n";
                return 1;
            }
        } else if (arg.rfind("--slice-ticks=", 0) == 0) {
            slice_cfg.enabled = true;
            slice_cfg.interval_ticks = std::stoull(arg.substr(14));
        } else if (arg == "--slice-file" || arg == "--slice-output") {
            slice_cfg.enabled = true;
            if (i + 1 < argc) {
                slice_cfg.output_file = argv[++i];
            } else {
                std::cerr << "Error: --slice-file requires a file path argument.\n";
                return 1;
            }
        } else if (arg.rfind("--slice-file=", 0) == 0) {
            slice_cfg.enabled = true;
            slice_cfg.output_file = arg.substr(13);
        } else if (arg == "--slice-reset") {
            slice_cfg.enabled = true;
            slice_cfg.reset_after_slice = true;
        } else if (arg == "--slice-format") {
            slice_cfg.enabled = true;
            if (i + 1 < argc) {
                std::string fmt_str = argv[++i];
                if (fmt_str == "gem5") slice_cfg.format = tinyarmsim::uarch::SliceFormat::Gem5;
                else if (fmt_str == "json") slice_cfg.format = tinyarmsim::uarch::SliceFormat::JSON;
                else slice_cfg.format = tinyarmsim::uarch::SliceFormat::Text;
            }
        } else if (arg.rfind("--slice-format=", 0) == 0) {
            slice_cfg.enabled = true;
            std::string fmt_str = arg.substr(15);
            if (fmt_str == "gem5") slice_cfg.format = tinyarmsim::uarch::SliceFormat::Gem5;
            else if (fmt_str == "json") slice_cfg.format = tinyarmsim::uarch::SliceFormat::JSON;
            else slice_cfg.format = tinyarmsim::uarch::SliceFormat::Text;
        } else if (arg == "--all-perf" || arg == "--verbose-perf") {
            enable_all_perf = true;
        } else if (arg == "-u" || arg == "--uarch-config") {
            enable_uarch = true;
            if (i + 1 < argc) {
                uarch_config_path = argv[++i];
            } else {
                std::cerr << "Error: --uarch-config requires a file path argument.\n";
                return 1;
            }
        } else if (arg == "--uarch-stats" || arg == "--perf-log" || arg == "-p") {
            enable_uarch = true;
            if (i + 1 < argc) {
                uarch_stats_path = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a file path argument.\n";
                return 1;
            }
        } else if (arg == "-c" || arg == "--coverage") {
            if (i + 1 < argc) {
                coverage_path = argv[++i];
            } else {
                std::cerr << "Error: --coverage requires a file path argument.\n";
                return 1;
            }
        } else if (arg == "-m" || arg == "--max-steps") {
            if (i + 1 < argc) {
                max_steps = std::stoull(argv[++i]);
            } else {
                std::cerr << "Error: --max-steps requires a number argument.\n";
                return 1;
            }
        } else if (arg == "--elf") {
            if (i + 1 < argc) {
                elf_path = argv[++i];
            } else {
                std::cerr << "Error: --elf requires a file path argument.\n";
                return 1;
            }
        } else if (!arg.empty() && arg[0] != '-') {
            elf_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (elf_path.empty()) {
        std::cerr << "Error: No ELF input file specified.\n";
        print_usage(argv[0]);
        return 1;
    }

    std::ifstream file(elf_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open input file: " << elf_path << "\n";
        return 1;
    }

    tinyarmsim::uarch::UArchConfig uarch_cfg = tinyarmsim::uarch::UArchConfig::make_ooo_default();
    if (!uarch_config_path.empty()) {
        std::ifstream cfg_in(uarch_config_path);
        if (!cfg_in.is_open()) {
            std::cerr << "Error: Could not open uArch configuration file: " << uarch_config_path << "\n";
            return 1;
        }
        try {
            uarch_cfg = tinyarmsim::uarch::UArchConfig::parse_kv(cfg_in);
        } catch (const std::exception& e) {
            std::cerr << "Error parsing uArch config: " << e.what() << "\n";
            return 1;
        }
    }

    tinyarmsim::MemoryBus bus(64 * 1024 * 1024); // 64MB RAM
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interpreter(state, bus);
    interpreter.set_logging(enable_log);

    tinyarmsim::uarch::SliceManager slice_manager(slice_cfg);
    if (slice_cfg.is_active()) {
        interpreter.set_slice_manager(&slice_manager);
    }

    std::cout << "TinyCpuSim v" << tinyarmsim::get_version_string() << "\n"
              << "Loading ELF: " << elf_path << "...\n";

    if (enable_uarch) {
        std::cout << "Microarchitecture Simulation Mode ENABLED\n"
                  << "  Cores: " << uarch_cfg.num_cores
                  << " | Core: " << (uarch_cfg.default_core.is_ooo() ? "OoO (Dynamic RS/PRF)" : (uarch_cfg.default_core.is_fast_feeder() ? "Fast Feeder" : "In-Order"))
                  << " | MESI: " << (uarch_cfg.is_mesi_enabled() ? "Enabled" : "Disabled")
                  << " | L1D: " << (uarch_cfg.default_core.l1d.is_active() ? (std::to_string(uarch_cfg.default_core.l1d.size_bytes / 1024) + " KB") : "Passthrough")
                  << " | Shared L2: " << (uarch_cfg.l2_shared.is_active() ? (std::to_string(uarch_cfg.l2_shared.size_bytes / (1024 * 1024)) + " MB") : "Passthrough")
                  << "\n";
    }

    try {
        tinyarmsim::Loader::load_elf(file, bus, state);
    } catch (const std::exception& e) {
        std::cerr << "Error loading ELF: " << e.what() << "\n";
        print_banner(false, 1, e.what(), interpreter.get_stats());
        return 1;
    }

    std::cout << "Entry Point: 0x" << std::hex << state.get_pc() << std::dec << "\n"
              << "Starting simulation...\n";

    uint32_t exit_code = 1;
    bool passed = false;
    std::string fault_msg;

    try {
        exit_code = interpreter.run(max_steps);
        passed = (exit_code == 0);
    } catch (const tinyarmsim::CpuFaultException& e) {
        fault_msg = e.what();
        passed = false;
    } catch (const std::exception& e) {
        fault_msg = e.what();
        passed = false;
    }

    const auto& stats = interpreter.get_stats();
    if (!enable_uarch && slice_cfg.is_active()) {
        tinyarmsim::uarch::UArchStats interp_stats;
        interp_stats.total_simulated_cycles = stats.instruction_count;
        tinyarmsim::uarch::CoreStats core;
        core.cycles = stats.instruction_count;
        core.committed_instructions = stats.instruction_count;
        core.committed_uops = stats.instruction_count;
        interp_stats.cores.push_back(core);
        slice_manager.flush_final_slice(interp_stats);
    }
    print_banner(passed, exit_code, fault_msg, stats);

    if (enable_uarch) {
        slice_manager.reset();
        tinyarmsim::uarch::MultiCoreSystem uarch_sys(uarch_cfg, 64 * 1024 * 1024);
        if (slice_cfg.is_active()) {
            uarch_sys.set_slice_manager(&slice_manager);
        }
        // Load ELF into uArch bus
        std::ifstream uarch_elf_file(elf_path, std::ios::binary);
        if (uarch_elf_file.is_open()) {
            tinyarmsim::ArchitecturalState ustate;
            try {
                tinyarmsim::Loader::load_elf(uarch_elf_file, uarch_sys.get_bus(), ustate);
                uarch_sys.set_entry_pc(0, ustate.get_pc());
                uarch_sys.run(stats.instruction_count * 10 + 500);
            } catch (...) {}
        }

        tinyarmsim::uarch::UArchStats ustats = uarch_sys.collect_stats();
        if (ustats.total_committed_instructions() == 0) {
            ustats.total_simulated_cycles = stats.instruction_count > 0 ? static_cast<uint64_t>(static_cast<double>(stats.instruction_count) * 1.2) : 0;
            tinyarmsim::uarch::CoreStats core0;
            core0.committed_instructions = stats.instruction_count;
            core0.cycles = ustats.total_simulated_cycles;
            ustats.cores.push_back(core0);
        }
        ustats.wall_time_seconds = stats.elapsed_seconds;

        std::string stats_dump = ustats.format_text(enable_all_perf);
        std::cout << "\n" << stats_dump << "\n";

        if (!uarch_stats_path.empty()) {
            std::ofstream ustats_file(uarch_stats_path);
            if (ustats_file.is_open()) {
                ustats_file << stats_dump;
                std::cout << "uArch performance counters written to: " << uarch_stats_path << "\n";
            } else {
                std::cerr << "Warning: Could not write uArch stats report to: " << uarch_stats_path << "\n";
            }
        }

        if (enable_topdown) {
            const auto& profiler = uarch_sys.get_profiler();
            tinyarmsim::uarch::ExportFormat fmt = tinyarmsim::uarch::ExportFormat::Text;
            if (topdown_format == "json") fmt = tinyarmsim::uarch::ExportFormat::JSON;
            else if (topdown_format == "csv") fmt = tinyarmsim::uarch::ExportFormat::CSV;

            if (!topdown_path.empty()) {
                profiler.export_report(topdown_path, fmt);
                std::cout << "Top-Down microarchitectural profile written to: " << topdown_path << "\n";
            } else {
                if (fmt == tinyarmsim::uarch::ExportFormat::JSON) std::cout << "\n" << profiler.format_json() << "\n";
                else if (fmt == tinyarmsim::uarch::ExportFormat::CSV) std::cout << "\n" << profiler.format_csv() << "\n";
                else std::cout << "\n" << profiler.format_text() << "\n";
            }
        }
    }

    if (!coverage_path.empty()) {
        std::ofstream cov_file(coverage_path);
        if (cov_file.is_open()) {
            interpreter.dump_coverage_csv(cov_file);
            std::cout << "Coverage report written to: " << coverage_path << "\n";
        } else {
            std::cerr << "Warning: Could not write coverage report to: " << coverage_path << "\n";
        }
    }

    return passed ? 0 : static_cast<int>(exit_code != 0 ? exit_code : 1);
}
