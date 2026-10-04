#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace tinyarmsim::uarch {

enum class CoreType {
    OOO_DYNAMIC,   // Modern Out-of-Order dynamic scheduling (Unified RS/IQ + PRF + ROB)
    SIMPLE_INORDER,// Strict in-order execution
    FAST_FEEDER    // High-throughput fast feeder for memory/cache exploration
};

enum class ReplacementPolicy {
    LRU,
    FIFO,
    RANDOM
};

enum class WritePolicy {
    WRITE_BACK,
    WRITE_THROUGH
};

enum class PredictorType {
    NONE,
    IDEAL,
    BIMODAL,
    GSHARE,
    TAGE
};

enum class LsuType {
    SPECULATIVE_OOO,
    STRICT_INORDER,
    STRICT_WAIT = STRICT_INORDER,
    PASSTHROUGH
};

enum class CacheType {
    SET_ASSOCIATIVE,
    DIRECT_MAPPED,
    ZERO_LATENCY,
    PASSTHROUGH
};

enum class CoherenceProtocol {
    NONE,
    MESI
};

enum class PrefetcherType {
    NONE,
    NEXT_LINE,
    STRIDE,
    STREAM
};

struct CacheConfig {
    CacheType type{CacheType::SET_ASSOCIATIVE};
    size_t size_bytes{32768};        // Default: 32KB
    size_t line_size{64};            // Default: 64B
    size_t associativity{4};         // Default: 4-way
    uint32_t hit_latency_cycles{1};  // Default: 1 cycle
    ReplacementPolicy replacement{ReplacementPolicy::LRU};
    WritePolicy write_policy{WritePolicy::WRITE_BACK};
    size_t mshr_entries{8};          // Non-blocking MSHR capacity
    PrefetcherType prefetcher{PrefetcherType::NONE}; // Hardware prefetcher type
    size_t prefetch_distance{1};     // Prefetch lookahead distance (in cache lines)
    size_t prefetch_queue_size{8};   // Prefetch request queue capacity

    [[nodiscard]] bool is_prefetch_enabled() const noexcept {
        return prefetcher != PrefetcherType::NONE;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return type != CacheType::PASSTHROUGH;
    }

    [[nodiscard]] bool is_zero_latency() const noexcept {
        return type == CacheType::ZERO_LATENCY;
    }

    [[nodiscard]] size_t num_sets() const noexcept {
        if (line_size == 0 || associativity == 0) return 0;
        return size_bytes / (line_size * associativity);
    }

    void validate() const {
        if (!is_active()) return;
        if (type == CacheType::DIRECT_MAPPED && associativity != 1) {
            throw std::invalid_argument("DIRECT_MAPPED cache must have associativity = 1");
        }
        if (size_bytes == 0 || (size_bytes & (size_bytes - 1)) != 0) {
            throw std::invalid_argument("Cache size_bytes must be a non-zero power of 2");
        }
        if (line_size == 0 || (line_size & (line_size - 1)) != 0) {
            throw std::invalid_argument("Cache line_size must be a non-zero power of 2");
        }
        if (associativity == 0 || (associativity & (associativity - 1)) != 0) {
            throw std::invalid_argument("Cache associativity must be a non-zero power of 2");
        }
        if (size_bytes < line_size * associativity) {
            throw std::invalid_argument("Cache size_bytes must be >= line_size * associativity");
        }
        if (prefetcher != PrefetcherType::NONE) {
            if (prefetch_distance == 0) {
                throw std::invalid_argument("Prefetch distance must be > 0 when prefetcher is enabled");
            }
            if (prefetch_queue_size == 0) {
                throw std::invalid_argument("Prefetch queue size must be > 0 when prefetcher is enabled");
            }
        }
    }
};

struct BranchPredictorConfig {
    PredictorType type{PredictorType::TAGE};
    size_t table_size{4096};
    size_t btb_size{4096};
    size_t ras_size{32};
    size_t tage_tables{4};

    [[nodiscard]] bool is_active() const noexcept {
        return type != PredictorType::NONE;
    }

    [[nodiscard]] bool is_ideal() const noexcept {
        return type == PredictorType::IDEAL;
    }

    void validate() const {
        if (!is_active() || is_ideal()) return;
        if (table_size == 0 || (table_size & (table_size - 1)) != 0) {
            throw std::invalid_argument("Predictor table_size must be a power of 2");
        }
        if (btb_size == 0 || (btb_size & (btb_size - 1)) != 0) {
            throw std::invalid_argument("BTB size must be a power of 2");
        }
    }
};

struct LsuConfig {
    LsuType type{LsuType::SPECULATIVE_OOO};
    size_t lq_size{16};
    size_t sq_size{16};
    uint32_t store_forward_latency{1};
    uint32_t num_load_ports{2};
    uint32_t num_store_ports{1};

    [[nodiscard]] bool is_active() const noexcept {
        return type != LsuType::PASSTHROUGH;
    }

    [[nodiscard]] bool is_speculative() const noexcept {
        return type == LsuType::SPECULATIVE_OOO;
    }

    void validate() const {
        if (!is_active()) return;
        if (lq_size == 0 || sq_size == 0) {
            throw std::invalid_argument("LQ and SQ sizes must be > 0");
        }
    }
};

struct CoreConfig {
    CoreType type{CoreType::OOO_DYNAMIC};
    uint32_t fetch_width{4};
    uint32_t decode_width{4};
    uint32_t rename_width{4};
    uint32_t issue_width{4};
    uint32_t commit_width{4};
    size_t rob_size{64};
    size_t rs_size{32};
    size_t num_phys_regs{128};

    CacheConfig l1i{};
    CacheConfig l1d{};
    BranchPredictorConfig branch_predictor{};
    LsuConfig lsu{};

    [[nodiscard]] bool is_ooo() const noexcept {
        return type == CoreType::OOO_DYNAMIC;
    }

    [[nodiscard]] bool is_fast_feeder() const noexcept {
        return type == CoreType::FAST_FEEDER;
    }

    [[nodiscard]] bool is_simple_inorder() const noexcept {
        return type == CoreType::SIMPLE_INORDER;
    }

    void validate() const {
        if (fetch_width == 0 || decode_width == 0 || rename_width == 0 ||
            issue_width == 0 || commit_width == 0) {
            throw std::invalid_argument("Pipeline stage widths must be > 0");
        }
        if (is_ooo()) {
            if (rob_size == 0 || rs_size == 0) {
                throw std::invalid_argument("ROB and RS sizes must be > 0 when OoO is enabled");
            }
            if (num_phys_regs <= 16) {
                throw std::invalid_argument("num_phys_regs must be > 16 (architectural registers count)");
            }
        }
        l1i.validate();
        l1d.validate();
        branch_predictor.validate();
        lsu.validate();
    }
};

struct UArchConfig {
    size_t num_cores{1};
    CoreConfig default_core{};
    std::vector<CoreConfig> cores{};
    CacheConfig l2_shared{
        CacheType::SET_ASSOCIATIVE,
        524288,   // 512KB
        64,       // 64B line
        8,        // 8-way
        10,       // 10 cycles hit latency
        ReplacementPolicy::LRU,
        WritePolicy::WRITE_BACK,
        16        // 16 MSHR entries
    };
    CoherenceProtocol coherence{CoherenceProtocol::MESI};
    uint32_t dram_latency_cycles{80};

    [[nodiscard]] bool is_mesi_enabled() const noexcept {
        return coherence == CoherenceProtocol::MESI;
    }

    [[nodiscard]] const CoreConfig& get_core_config(size_t core_id) const noexcept {
        if (core_id < cores.size()) {
            return cores[core_id];
        }
        return default_core;
    }

    void validate() {
        if (num_cores == 0) {
            throw std::invalid_argument("num_cores must be >= 1");
        }
        default_core.validate();
        for (auto& c : cores) {
            c.validate();
        }
        l2_shared.validate();
    }

    static UArchConfig make_in_order_simple() {
        UArchConfig cfg;
        cfg.num_cores = 1;
        cfg.default_core.type = CoreType::SIMPLE_INORDER;
        cfg.default_core.fetch_width = 1;
        cfg.default_core.decode_width = 1;
        cfg.default_core.rename_width = 1;
        cfg.default_core.issue_width = 1;
        cfg.default_core.commit_width = 1;
        cfg.default_core.rob_size = 1;
        cfg.default_core.rs_size = 1;
        cfg.default_core.branch_predictor.type = PredictorType::BIMODAL;
        cfg.default_core.lsu.type = LsuType::PASSTHROUGH;
        cfg.default_core.l1i.type = CacheType::PASSTHROUGH;
        cfg.default_core.l1d.type = CacheType::PASSTHROUGH;
        cfg.l2_shared.type = CacheType::PASSTHROUGH;
        cfg.coherence = CoherenceProtocol::NONE;
        return cfg;
    }

    static UArchConfig make_ooo_default() {
        UArchConfig cfg;
        cfg.num_cores = 1;
        cfg.validate();
        return cfg;
    }

    static UArchConfig make_multicore_default(size_t cores = 4) {
        UArchConfig cfg;
        cfg.num_cores = cores;
        cfg.cores.resize(cores, cfg.default_core);
        cfg.l2_shared.size_bytes = 1024 * 1024 * 2; // 2MB
        cfg.coherence = CoherenceProtocol::MESI;
        cfg.validate();
        return cfg;
    }

    static UArchConfig make_fast_feeder_mem() {
        UArchConfig cfg;
        cfg.num_cores = 1;
        cfg.default_core.type = CoreType::FAST_FEEDER;
        cfg.default_core.branch_predictor.type = PredictorType::IDEAL;
        cfg.default_core.lsu.type = LsuType::PASSTHROUGH;
        cfg.default_core.l1i.type = CacheType::SET_ASSOCIATIVE;
        cfg.default_core.l1d.type = CacheType::SET_ASSOCIATIVE;
        cfg.l2_shared.type = CacheType::SET_ASSOCIATIVE;
        cfg.validate();
        return cfg;
    }

    // Key-Value style configuration parser (supports comments #, section headers [core], key=value)
    static UArchConfig parse_kv(std::istream& in) {
        UArchConfig cfg;
        std::string line;
        std::string current_section = "global";

        while (std::getline(in, line)) {
            // Strip comments
            auto hash_pos = line.find('#');
            if (hash_pos != std::string::npos) {
                line = line.substr(0, hash_pos);
            }
            // Trim whitespace
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            if (line.empty()) continue;

            if (line.front() == '[' && line.back() == ']') {
                current_section = line.substr(1, line.size() - 2);
                continue;
            }

            auto eq_pos = line.find('=');
            if (eq_pos == std::string::npos) continue;

            std::string key = line.substr(0, eq_pos);
            std::string val = line.substr(eq_pos + 1);

            auto trim_str = [](std::string& s) {
                auto s_start = s.find_first_not_of(" \t\r\n");
                if (s_start == std::string::npos) { s.clear(); return; }
                auto s_end = s.find_last_not_of(" \t\r\n");
                s = s.substr(s_start, s_end - s_start + 1);
            };
            trim_str(key);
            trim_str(val);

            auto to_upper = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), ::toupper);
                return s;
            };

            auto parse_bool = [](const std::string& v) -> bool {
                return (v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "True");
            };

            std::string u_val = to_upper(val);

            auto parse_cache_field = [&](CacheConfig& c) {
                if (key == "type") {
                    if (u_val == "SET_ASSOCIATIVE" || u_val == "DEFAULT") c.type = CacheType::SET_ASSOCIATIVE;
                    else if (u_val == "DIRECT_MAPPED") { c.type = CacheType::DIRECT_MAPPED; c.associativity = 1; }
                    else if (u_val == "ZERO_LATENCY" || u_val == "IDEAL") c.type = CacheType::ZERO_LATENCY;
                    else if (u_val == "PASSTHROUGH" || u_val == "NONE" || u_val == "BYPASS") c.type = CacheType::PASSTHROUGH;
                }
                else if (key == "enabled") {
                    c.type = parse_bool(val) ? CacheType::SET_ASSOCIATIVE : CacheType::PASSTHROUGH;
                }
                else if (key == "size_bytes" || key == "size") c.size_bytes = std::stoul(val);
                else if (key == "line_size") c.line_size = std::stoul(val);
                else if (key == "associativity" || key == "assoc") c.associativity = std::stoul(val);
                else if (key == "hit_latency" || key == "hit_latency_cycles") c.hit_latency_cycles = static_cast<uint32_t>(std::stoul(val));
                else if (key == "mshr_entries") c.mshr_entries = std::stoul(val);
                else if (key == "prefetcher" || key == "prefetch_type" || key == "prefetcher_type") {
                    if (u_val == "NONE" || u_val == "DISABLED") c.prefetcher = PrefetcherType::NONE;
                    else if (u_val == "NEXT_LINE" || u_val == "NEXTLINE") c.prefetcher = PrefetcherType::NEXT_LINE;
                    else if (u_val == "STRIDE") c.prefetcher = PrefetcherType::STRIDE;
                    else if (u_val == "STREAM") c.prefetcher = PrefetcherType::STREAM;
                }
                else if (key == "prefetch_distance") c.prefetch_distance = std::stoul(val);
                else if (key == "prefetch_queue_size") c.prefetch_queue_size = std::stoul(val);
            };

            if (current_section == "global" || current_section == "system") {
                if (key == "num_cores") cfg.num_cores = std::stoul(val);
                else if (key == "coherence" || key == "coherence_protocol") {
                    if (u_val == "MESI") cfg.coherence = CoherenceProtocol::MESI;
                    else if (u_val == "NONE" || u_val == "DISABLED") cfg.coherence = CoherenceProtocol::NONE;
                }
                else if (key == "enable_mesi" || key == "enable_mesi_coherence") {
                    cfg.coherence = parse_bool(val) ? CoherenceProtocol::MESI : CoherenceProtocol::NONE;
                }
                else if (key == "dram_latency" || key == "dram_latency_cycles") cfg.dram_latency_cycles = static_cast<uint32_t>(std::stoul(val));
            } else if (current_section == "core") {
                if (key == "type" || key == "mode") {
                    if (u_val == "OOO_DYNAMIC" || u_val == "OOO_RS_PRF" || u_val == "OOO_TOMASULO" || u_val == "OOO") cfg.default_core.type = CoreType::OOO_DYNAMIC;
                    else if (u_val == "SIMPLE_INORDER" || u_val == "INORDER" || u_val == "IN_ORDER") cfg.default_core.type = CoreType::SIMPLE_INORDER;
                    else if (u_val == "FAST_FEEDER" || u_val == "FEEDER" || u_val == "BYPASS") cfg.default_core.type = CoreType::FAST_FEEDER;
                }
                else if (key == "enable_ooo" || key == "ooo") {
                    cfg.default_core.type = parse_bool(val) ? CoreType::OOO_DYNAMIC : CoreType::SIMPLE_INORDER;
                }
                else if (key == "fetch_width") cfg.default_core.fetch_width = static_cast<uint32_t>(std::stoul(val));
                else if (key == "decode_width") cfg.default_core.decode_width = static_cast<uint32_t>(std::stoul(val));
                else if (key == "rename_width") cfg.default_core.rename_width = static_cast<uint32_t>(std::stoul(val));
                else if (key == "issue_width") cfg.default_core.issue_width = static_cast<uint32_t>(std::stoul(val));
                else if (key == "commit_width") cfg.default_core.commit_width = static_cast<uint32_t>(std::stoul(val));
                else if (key == "rob_size" || key == "rob") cfg.default_core.rob_size = std::stoul(val);
                else if (key == "rs_size" || key == "rs" || key == "iq_size") cfg.default_core.rs_size = std::stoul(val);
                else if (key == "num_phys_regs" || key == "prf") cfg.default_core.num_phys_regs = std::stoul(val);
            } else if (current_section == "l1i" || current_section == "cache_l1i") {
                parse_cache_field(cfg.default_core.l1i);
            } else if (current_section == "l1d" || current_section == "cache_l1d") {
                parse_cache_field(cfg.default_core.l1d);
            } else if (current_section == "l2" || current_section == "cache_l2") {
                parse_cache_field(cfg.l2_shared);
            } else if (current_section == "branch_predictor") {
                if (key == "type") {
                    if (u_val == "IDEAL" || u_val == "BYPASS") cfg.default_core.branch_predictor.type = PredictorType::IDEAL;
                    else if (u_val == "NONE" || u_val == "DISABLED") cfg.default_core.branch_predictor.type = PredictorType::NONE;
                    else if (u_val == "BIMODAL" || u_val == "BIMODE" || u_val == "BIMODEBP") cfg.default_core.branch_predictor.type = PredictorType::BIMODAL;
                    else if (u_val == "GSHARE") cfg.default_core.branch_predictor.type = PredictorType::GSHARE;
                    else if (u_val == "TAGE") cfg.default_core.branch_predictor.type = PredictorType::TAGE;
                }
                else if (key == "enabled") {
                    if (!parse_bool(val)) cfg.default_core.branch_predictor.type = PredictorType::NONE;
                }
                else if (key == "table_size") cfg.default_core.branch_predictor.table_size = std::stoul(val);
                else if (key == "btb_size") cfg.default_core.branch_predictor.btb_size = std::stoul(val);
                else if (key == "ras_size") cfg.default_core.branch_predictor.ras_size = std::stoul(val);
                else if (key == "tage_tables") cfg.default_core.branch_predictor.tage_tables = std::stoul(val);
            } else if (current_section == "lsu") {
                if (key == "type") {
                    if (u_val == "SPECULATIVE_OOO" || u_val == "OOO" || u_val == "SPECULATIVE") cfg.default_core.lsu.type = LsuType::SPECULATIVE_OOO;
                    else if (u_val == "STRICT_INORDER" || u_val == "INORDER" || u_val == "STRICT_WAIT" || u_val == "STRICT") cfg.default_core.lsu.type = LsuType::STRICT_INORDER;
                    else if (u_val == "PASSTHROUGH" || u_val == "NONE" || u_val == "BYPASS") cfg.default_core.lsu.type = LsuType::PASSTHROUGH;
                }
                else if (key == "enabled") {
                    if (!parse_bool(val)) cfg.default_core.lsu.type = LsuType::PASSTHROUGH;
                }
                else if (key == "lq_size") cfg.default_core.lsu.lq_size = std::stoul(val);
                else if (key == "sq_size") cfg.default_core.lsu.sq_size = std::stoul(val);
                else if (key == "store_forward_latency") cfg.default_core.lsu.store_forward_latency = static_cast<uint32_t>(std::stoul(val));
            } else if (current_section == "prefetcher" || current_section == "prefetch") {
                if (key == "type" || key == "prefetcher" || key == "prefetcher_type") {
                    PrefetcherType pt = PrefetcherType::NONE;
                    if (u_val == "NONE" || u_val == "DISABLED") pt = PrefetcherType::NONE;
                    else if (u_val == "NEXT_LINE" || u_val == "NEXTLINE") pt = PrefetcherType::NEXT_LINE;
                    else if (u_val == "STRIDE") pt = PrefetcherType::STRIDE;
                    else if (u_val == "STREAM") pt = PrefetcherType::STREAM;
                    cfg.default_core.l1d.prefetcher = pt;
                }
                else if (key == "enabled") {
                    if (!parse_bool(val)) cfg.default_core.l1d.prefetcher = PrefetcherType::NONE;
                }
                else if (key == "distance" || key == "prefetch_distance") {
                    cfg.default_core.l1d.prefetch_distance = std::stoul(val);
                }
                else if (key == "queue_size" || key == "prefetch_queue_size") {
                    cfg.default_core.l1d.prefetch_queue_size = std::stoul(val);
                }
            }
        }

        if (cfg.num_cores > 1 && cfg.cores.empty()) {
            cfg.cores.resize(cfg.num_cores, cfg.default_core);
        }

        cfg.validate();
        return cfg;
    }
};

} // namespace tinyarmsim::uarch
