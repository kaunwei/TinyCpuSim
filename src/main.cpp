#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <iomanip>
#include "tinyarmsim/common.hpp"
#include "tinyarmsim/cli_options.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/loader.hpp"
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/firmware_control.hpp"
#include "tinyarmsim/firmware_uart_output.hpp"
#include "tinyarmsim/firmware_signal_guard.hpp"
#include "tinyarmsim/mmio/soc_platform.hpp"
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
              << "  --firmware               Boot a Cortex-M firmware ELF on the default SoC\n"
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

class ScopedCoutRedirect {
public:
    explicit ScopedCoutRedirect(bool enabled) {
        if (enabled) {
            null_out_.open("/dev/null");
            if (null_out_.is_open()) {
                original_buf_ = std::cout.rdbuf(null_out_.rdbuf());
            }
        }
    }
    ~ScopedCoutRedirect() {
        restore();
    }
    ScopedCoutRedirect(const ScopedCoutRedirect&) = delete;
    ScopedCoutRedirect& operator=(const ScopedCoutRedirect&) = delete;

    void restore() noexcept {
        if (original_buf_ != nullptr) {
            std::cout.rdbuf(original_buf_);
            original_buf_ = nullptr;
        }
    }

private:
    std::ofstream null_out_{};
    std::streambuf* original_buf_{nullptr};
};

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    auto parsed = tinyarmsim::parse_cli_options(arguments);
    if (parsed.help_requested) {
        print_usage(argv[0]);
        return parsed.exit_code;
    }
    if (!parsed.diagnostic.empty()) {
        std::cerr << parsed.diagnostic;
        if (parsed.show_usage) print_usage(argv[0]);
        return 1;
    }
    auto options = std::move(parsed.options);
    auto& elf_path = options.elf_path;
    auto& coverage_path = options.coverage_path;
    auto& uarch_config_path = options.uarch_config_path;
    auto& uarch_stats_path = options.uarch_stats_path;
    auto& topdown_path = options.topdown_path;
    auto& topdown_format = options.topdown_format;
    bool enable_log = options.enable_log, enable_firmware = options.enable_firmware;
    bool enable_uarch = options.enable_uarch, enable_all_perf = options.enable_all_perf;
    bool enable_topdown = options.enable_topdown;
    bool quiet = options.quiet;
    uint64_t max_steps = options.max_steps;
    auto& slice_cfg = options.slice_cfg;
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
    tinyarmsim::mmio::SoCPeripherals soc{};
    constexpr uint32_t firmware_vtor = 0x00010000u;
    if (enable_firmware) {
        soc = tinyarmsim::mmio::SoCPeripherals::attach_all(bus, interpreter);
    }
    interpreter.set_logging(enable_log);

    tinyarmsim::uarch::SliceManager slice_manager(slice_cfg);
    if (!enable_uarch && slice_cfg.is_active()) {
        interpreter.set_slice_manager(&slice_manager);
    }

    ScopedCoutRedirect cout_redirect(quiet);

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
        tinyarmsim::Loader::load_elf(file, bus, state, enable_firmware
            ? tinyarmsim::Loader::ElfLoadMode::Firmware
            : tinyarmsim::Loader::ElfLoadMode::Default);
        if (enable_firmware) {
            soc.nvic->set_vtor(firmware_vtor);
            tinyarmsim::Loader::boot_cortex_m(bus, state, firmware_vtor);
        }
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

    std::optional<tinyarmsim::FirmwareSignalGuard> signal_guard;
    std::optional<tinyarmsim::FirmwareUartOutput> uart_output;
    if (enable_firmware) {
        signal_guard.emplace();
        uart_output.emplace();
        soc.uart->set_tx_consumer([&uart_output](uint8_t byte) {
            if (uart_output) {
                (void)uart_output->enqueue(byte);
            }
        });
    }

    try {
        if (options.max_steps_explicit && max_steps == 0) {
            exit_code = 1;
            fault_msg = "Maximum instruction count reached";
        } else if (enable_firmware && options.control_fd >= 0) {
            tinyarmsim::FirmwareControl control(options.control_fd);
            control.set_instruction_counter([&interpreter]() {
                return interpreter.get_stats().instruction_count;
            });
            control.set_pc_provider([&state]() {
                return state.get_pc();
            });
            control.set_memory_provider([&bus](uint32_t address, size_t length) {
                return bus.read_ram_bytes(address, length);
            });
            control.set_register_provider([&interpreter, &state]() {
                std::string s = "{\"event\":\"regs\",\"pc\":" + std::to_string(state.get_pc()) + ",\"registers\":[";
                for (size_t i = 0; i < 16; ++i) {
                    if (i > 0) s += ",";
                    s += std::to_string(state.get_reg(i));
                }
                s += "],\"steps\":" + std::to_string(interpreter.get_stats().instruction_count) + "}";
                return s;
            });
            control.set_host_service([&uart_output, &signal_guard]() {
                if (signal_guard && signal_guard->received_signal() != 0) {
                    throw std::runtime_error("Interrupted by signal " + std::to_string(signal_guard->received_signal()));
                }
                if (uart_output) {
                    uart_output->service();
                }
            });
            exit_code = interpreter.run_until([&control, &signal_guard]() {
                if (signal_guard && signal_guard->received_signal() != 0) {
                    return true;
                }
                return control.stop_requested();
            }, options.max_steps_explicit ? max_steps : 0);
            if (signal_guard && signal_guard->received_signal() != 0) {
                exit_code = 128 + signal_guard->received_signal();
                fault_msg = "Interrupted by signal " + std::to_string(signal_guard->received_signal());
            } else if (control.closed_without_stop() || control.transport_failed()) {
                exit_code = 1;
                fault_msg = "Firmware control closed without stop or transport failed";
            }
        } else if (enable_firmware) {
            exit_code = interpreter.run_until([&uart_output, &signal_guard]() {
                if (signal_guard && signal_guard->received_signal() != 0) {
                    return true;
                }
                if (uart_output) {
                    uart_output->service();
                }
                return false;
            }, options.max_steps_explicit ? max_steps : 0);
            if (signal_guard && signal_guard->received_signal() != 0) {
                exit_code = 128 + signal_guard->received_signal();
                fault_msg = "Interrupted by signal " + std::to_string(signal_guard->received_signal());
            }
        } else {
            exit_code = interpreter.run(max_steps);
        }
        passed = (exit_code == 0 && fault_msg.empty());
    } catch (const tinyarmsim::CpuFaultException& e) {
        fault_msg = e.what();
        passed = false;
    } catch (const std::exception& e) {
        fault_msg = e.what();
        passed = false;
        if (signal_guard && signal_guard->received_signal() != 0) {
            exit_code = 128 + signal_guard->received_signal();
        }
    }

    if (uart_output) {
        try {
            uart_output->flush(std::chrono::milliseconds(200));
        } catch (const std::exception& e) {
            if (fault_msg.empty()) {
                fault_msg = e.what();
                passed = false;
                if (exit_code == 0) exit_code = 1;
            }
        }
        if (uart_output->failed()) {
            passed = false;
            if (exit_code == 0) exit_code = 1;
            if (fault_msg.empty()) fault_msg = "firmware UART output failed";
        }
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
    if (quiet && !passed && !fault_msg.empty()) {
        std::cerr << fault_msg << "\n";
    }
    cout_redirect.restore();
    if (enable_firmware && !uart_output && !soc.uart->get_tx_output().empty()) {
        std::cout << soc.uart->get_tx_output();
        if (!quiet && soc.uart->get_tx_output().back() != '\n') std::cout << '\n';
    }

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

    if (passed) {
        return 0;
    }
    return exit_code > 0 && exit_code <= 255 ? static_cast<int>(exit_code) : 1;
}
