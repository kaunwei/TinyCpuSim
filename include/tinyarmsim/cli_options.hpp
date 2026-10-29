#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "tinyarmsim/uarch/slice_manager.hpp"

namespace tinyarmsim {
struct CliOptions {
    std::string elf_path, coverage_path, uarch_config_path, uarch_stats_path, topdown_path;
    std::string topdown_format = "text";
    bool enable_log = false, enable_firmware = false, enable_uarch = false;
    bool enable_all_perf = false, enable_topdown = false, quiet = false;
    int control_fd = -1;
    int uart_fd = -1;
    bool max_steps_explicit = false;
    uint64_t max_steps = 1000000000;
    uarch::SliceConfig slice_cfg;
};
struct CliParseResult {
    CliOptions options;
    int exit_code = -1;
    bool help_requested = false;
    bool show_usage = false;
    std::string diagnostic;
};
inline uarch::SliceFormat parse_slice_format(const std::string& format) {
    if (format == "gem5") return uarch::SliceFormat::Gem5;
    if (format == "json") return uarch::SliceFormat::JSON;
    return uarch::SliceFormat::Text;
}
inline CliParseResult parse_cli_options(const std::vector<std::string>& args) {
    CliParseResult result;
    auto& options = result.options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            result.help_requested = true;
            result.exit_code = 0;
            return result;
        } else if (arg == "-l" || arg == "--log" || arg == "--verbose") {
            options.enable_log = true;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else if (arg == "--firmware") {
            options.enable_firmware = true;
        } else if (arg == "--uarch" || arg == "--uarch-mode") {
            options.enable_uarch = true;
        } else if (arg == "--topdown") {
            options.enable_uarch = true;
            options.enable_topdown = true;
        } else if (arg.rfind("--topdown=", 0) == 0) {
            options.enable_uarch = true;
            options.enable_topdown = true;
            options.topdown_path = arg.substr(10);
        } else if (arg == "--topdown-file" || arg == "--topdown-report") {
            options.enable_uarch = true;
            options.enable_topdown = true;
            if (i + 1 < args.size()) {
                options.topdown_path = args[++i];
            } else {
                result.diagnostic = "Error: --topdown-file requires a file path argument.\n";
                return result;
            }
        } else if (arg == "--topdown-format") {
            if (i + 1 < args.size()) {
                options.topdown_format = args[++i];
            }
        } else if (arg.rfind("--topdown-format=", 0) == 0) {
            options.topdown_format = arg.substr(17);
        } else if (arg == "--slice-insts" || arg == "--slice-instructions") {
            options.slice_cfg.enabled = true;
            if (i + 1 < args.size()) {
                options.slice_cfg.interval_instructions = std::stoull(args[++i]);
            } else {
                result.diagnostic = "Error: --slice-insts requires a number argument.\n";
                return result;
            }
        } else if (arg.rfind("--slice-insts=", 0) == 0) {
            options.slice_cfg.enabled = true;
            options.slice_cfg.interval_instructions = std::stoull(arg.substr(14));
        } else if (arg == "--slice-ticks" || arg == "--slice-cycles") {
            options.slice_cfg.enabled = true;
            if (i + 1 < args.size()) {
                options.slice_cfg.interval_ticks = std::stoull(args[++i]);
            } else {
                result.diagnostic = "Error: --slice-ticks requires a number argument.\n";
                return result;
            }
        } else if (arg.rfind("--slice-ticks=", 0) == 0) {
            options.slice_cfg.enabled = true;
            options.slice_cfg.interval_ticks = std::stoull(arg.substr(14));
        } else if (arg == "--slice-file" || arg == "--slice-output") {
            options.slice_cfg.enabled = true;
            if (i + 1 < args.size()) {
                options.slice_cfg.output_file = args[++i];
            } else {
                result.diagnostic = "Error: --slice-file requires a file path argument.\n";
                return result;
            }
        } else if (arg.rfind("--slice-file=", 0) == 0) {
            options.slice_cfg.enabled = true;
            options.slice_cfg.output_file = arg.substr(13);
        } else if (arg == "--slice-reset") {
            options.slice_cfg.enabled = true;
            options.slice_cfg.reset_after_slice = true;
        } else if (arg == "--slice-format") {
            options.slice_cfg.enabled = true;
            if (i + 1 < args.size()) {
                std::string fmt_str = args[++i];
                options.slice_cfg.format = parse_slice_format(fmt_str);
            }
        } else if (arg.rfind("--slice-format=", 0) == 0) {
            options.slice_cfg.enabled = true;
            std::string fmt_str = arg.substr(15);
            options.slice_cfg.format = parse_slice_format(fmt_str);
        } else if (arg == "--all-perf" || arg == "--verbose-perf") {
            options.enable_all_perf = true;
        } else if (arg == "-u" || arg == "--uarch-config") {
            options.enable_uarch = true;
            if (i + 1 < args.size()) {
                options.uarch_config_path = args[++i];
            } else {
                result.diagnostic = "Error: --uarch-config requires a file path argument.\n";
                return result;
            }
        } else if (arg == "--uarch-stats" || arg == "--perf-log" || arg == "-p") {
            options.enable_uarch = true;
            if (i + 1 < args.size()) {
                options.uarch_stats_path = args[++i];
            } else {
                result.diagnostic = "Error: " + arg + " requires a file path argument.\n";
                return result;
            }
        } else if (arg == "-c" || arg == "--coverage") {
            if (i + 1 < args.size()) {
                options.coverage_path = args[++i];
            } else {
                result.diagnostic = "Error: --coverage requires a file path argument.\n";
                return result;
            }
        } else if (arg == "-m" || arg == "--max-steps") {
            if (i + 1 < args.size()) {
                options.max_steps = std::stoull(args[++i]);
                options.max_steps_explicit = true;
            } else {
                result.diagnostic = "Error: --max-steps requires a number argument.\n";
                return result;
            }
        } else if (arg == "--control-fd") {
            if (i + 1 < args.size()) {
                const std::string& val_str = args[++i];
                try {
                    size_t pos = 0;
                    long long val = std::stoll(val_str, &pos);
                    if (pos != val_str.size() || val < 0 || val > std::numeric_limits<int>::max()) {
                        result.diagnostic = "Error: --control-fd requires a nonnegative integer argument.\n";
                        return result;
                    }
                    options.control_fd = static_cast<int>(val);
                } catch (...) {
                    result.diagnostic = "Error: --control-fd requires a nonnegative integer argument.\n";
                    return result;
                }
            } else {
                result.diagnostic = "Error: --control-fd requires a nonnegative integer argument.\n";
                return result;
            }
        } else if (arg == "--uart-fd") {
            if (i + 1 < args.size()) {
                const std::string& val_str = args[++i];
                try {
                    size_t pos = 0;
                    long long val = std::stoll(val_str, &pos);
                    if (pos != val_str.size() || val < 0 || val > std::numeric_limits<int>::max()) {
                        result.diagnostic = "Error: --uart-fd requires a nonnegative integer argument.\n";
                        return result;
                    }
                    options.uart_fd = static_cast<int>(val);
                } catch (...) {
                    result.diagnostic = "Error: --uart-fd requires a nonnegative integer argument.\n";
                    return result;
                }
            } else {
                result.diagnostic = "Error: --uart-fd requires a nonnegative integer argument.\n";
                return result;
            }
        } else if (arg == "--elf") {
            if (i + 1 < args.size()) {
                options.elf_path = args[++i];
            } else {
                result.diagnostic = "Error: --elf requires a file path argument.\n";
                return result;
            }
        } else if (!arg.empty() && arg[0] != '-') {
            options.elf_path = arg;
        } else {
            result.diagnostic = "Unknown option: " + arg + "\n";
            result.show_usage = true;
            return result;
        }
    }

    if (options.control_fd >= 0 && !options.enable_firmware) {
        result.diagnostic = "Error: --control-fd requires --firmware mode.\n";
        return result;
    }
    if (options.control_fd >= 0 && options.max_steps_explicit && options.max_steps == 0) {
        result.diagnostic = "Error: --max-steps 0 cannot be used with --control-fd.\n";
        return result;
    }

    return result;
}
} // namespace tinyarmsim
