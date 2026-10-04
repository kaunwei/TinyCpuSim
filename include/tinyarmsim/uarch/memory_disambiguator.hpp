#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <limits>

namespace tinyarmsim::uarch {

/**
 * @brief Abstract interface for Memory Disambiguation and Load Speculation Predictors.
 */
class IMemoryDisambiguator {
public:
    virtual ~IMemoryDisambiguator() = default;

    virtual void insert_load(uint32_t load_pc, uint64_t load_seq_num) = 0;
    virtual void insert_store(uint32_t store_pc, uint64_t store_seq_num) = 0;
    [[nodiscard]] virtual uint64_t check_dep(uint32_t load_pc) = 0;
    [[nodiscard]] virtual bool can_bypass(uint32_t load_pc) = 0;
    virtual void record_violation(uint32_t store_pc, uint32_t load_pc) = 0;
    virtual void store_issued(uint32_t store_pc, uint64_t store_seq_num) = 0;
    virtual void squash(uint64_t squashed_seq_num) = 0;
    virtual void clear() = 0;
};

/**
 * @brief Blind Disambiguator: always speculatively predicts that loads do not
 * depend on any in-flight stores (always allows bypass).
 */
class BlindDisambiguator : public IMemoryDisambiguator {
public:
    void insert_load([[maybe_unused]] uint32_t load_pc, [[maybe_unused]] uint64_t load_seq_num) override {}
    void insert_store([[maybe_unused]] uint32_t store_pc, [[maybe_unused]] uint64_t store_seq_num) override {}

    [[nodiscard]] uint64_t check_dep([[maybe_unused]] uint32_t load_pc) override {
        return 0; // 0 indicates no predicted dependency
    }

    [[nodiscard]] bool can_bypass([[maybe_unused]] uint32_t load_pc) override {
        return true;
    }

    void record_violation([[maybe_unused]] uint32_t store_pc, [[maybe_unused]] uint32_t load_pc) override {}
    void store_issued([[maybe_unused]] uint32_t store_pc, [[maybe_unused]] uint64_t store_seq_num) override {}
    void squash([[maybe_unused]] uint64_t squashed_seq_num) override {}
    void clear() override {}
};

/**
 * @brief Store Sets Disambiguator (Chrysos and Emer, ISCA 1998).
 * Uses a Store Set ID Table (SSIT) and Last Fetched Store Table (LFST)
 * to dynamically correlate dependent Load/Store pairs, eliminating memory
 * order violation replays while allowing independent loads to aggressively bypass.
 */
class StoreSetsDisambiguator : public IMemoryDisambiguator {
public:
    static constexpr uint32_t INVALID_SSID = std::numeric_limits<uint32_t>::max();

    explicit StoreSetsDisambiguator(size_t ssit_entries = 1024,
                                   size_t lfst_entries = 256,
                                   uint64_t clear_period = 1000000)
        : ssit_capacity_(ssit_entries),
          lfst_size_(lfst_entries > 0 ? lfst_entries : 256),
          clear_period_(clear_period),
          lfst_(lfst_size_, 0),
          valid_lfst_(lfst_size_, false) {}

    [[nodiscard]] bool has_ssid(uint32_t pc) const noexcept {
        auto it = ssit_.find(pc);
        return it != ssit_.end() && it->second != INVALID_SSID;
    }

    [[nodiscard]] uint32_t get_ssid(uint32_t pc) const {
        auto it = ssit_.find(pc);
        if (it != ssit_.end()) {
            return it->second;
        }
        return INVALID_SSID;
    }

    void set_ssid(uint32_t pc, uint32_t ssid) {
        ssit_[pc] = ssid;
    }

    void insert_load([[maybe_unused]] uint32_t load_pc, [[maybe_unused]] uint64_t load_seq_num) override {
        check_clear();
    }

    void insert_store(uint32_t store_pc, uint64_t store_seq_num) override {
        check_clear();

        if (!has_ssid(store_pc)) {
            return;
        }

        uint32_t ssid = get_ssid(store_pc);
        if (ssid < lfst_size_) {
            lfst_[ssid] = store_seq_num;
            valid_lfst_[ssid] = true;
            store_list_[store_seq_num] = ssid;
        }
    }

    [[nodiscard]] uint64_t check_dep(uint32_t load_pc) override {
        if (!has_ssid(load_pc)) {
            return 0;
        }

        uint32_t ssid = get_ssid(load_pc);
        if (ssid < lfst_size_ && valid_lfst_[ssid]) {
            return lfst_[ssid];
        }

        return 0;
    }

    [[nodiscard]] bool can_bypass(uint32_t load_pc) override {
        return check_dep(load_pc) == 0;
    }

    void store_issued(uint32_t store_pc, uint64_t store_seq_num) override {
        store_list_.erase(store_seq_num);

        if (!has_ssid(store_pc)) {
            return;
        }

        uint32_t ssid = get_ssid(store_pc);
        if (ssid < lfst_size_ && valid_lfst_[ssid] && lfst_[ssid] == store_seq_num) {
            valid_lfst_[ssid] = false;
        }
    }

    void record_violation(uint32_t store_pc, uint32_t load_pc) override {
        bool valid_load = has_ssid(load_pc);
        bool valid_store = has_ssid(store_pc);

        if (!valid_load && !valid_store) {
            uint32_t new_set = calc_ssid(load_pc);
            set_ssid(load_pc, new_set);
            set_ssid(store_pc, new_set);
        } else if (valid_load && !valid_store) {
            uint32_t load_ssid = get_ssid(load_pc);
            set_ssid(store_pc, load_ssid);
        } else if (!valid_load && valid_store) {
            uint32_t store_ssid = get_ssid(store_pc);
            set_ssid(load_pc, store_ssid);
        } else {
            uint32_t load_ssid = get_ssid(load_pc);
            uint32_t store_ssid = get_ssid(store_pc);

            uint32_t min_ssid = std::min(load_ssid, store_ssid);
            set_ssid(load_pc, min_ssid);
            set_ssid(store_pc, min_ssid);
        }
    }

    void squash(uint64_t squashed_seq_num) override {
        auto it = store_list_.begin();
        while (it != store_list_.end()) {
            uint64_t seq = it->first;
            uint32_t ssid = it->second;

            if (seq <= squashed_seq_num) {
                break;
            }

            if (ssid < lfst_size_ && valid_lfst_[ssid] && lfst_[ssid] > squashed_seq_num) {
                valid_lfst_[ssid] = false;
            }

            it = store_list_.erase(it);
        }
    }

    void clear() override {
        ssit_.clear();
        std::fill(valid_lfst_.begin(), valid_lfst_.end(), false);
        std::fill(lfst_.begin(), lfst_.end(), 0);
        store_list_.clear();
        mem_ops_count_ = 0;
    }

    [[nodiscard]] size_t get_ssit_capacity() const noexcept { return ssit_capacity_; }
    [[nodiscard]] size_t get_lfst_size() const noexcept { return lfst_size_; }

private:
    [[nodiscard]] inline uint32_t calc_ssid(uint32_t pc) const noexcept {
        return static_cast<uint32_t>(((pc ^ (pc >> 10)) % lfst_size_));
    }

    void check_clear() {
        mem_ops_count_++;
        if (clear_period_ > 0 && mem_ops_count_ > clear_period_) {
            mem_ops_count_ = 0;
            clear();
        }
    }

    size_t ssit_capacity_{1024};
    size_t lfst_size_{256};
    uint64_t clear_period_{1000000};
    uint64_t mem_ops_count_{0};

    std::unordered_map<uint32_t, uint32_t> ssit_{};
    std::vector<uint64_t> lfst_{};
    std::vector<bool> valid_lfst_{};
    std::map<uint64_t, uint32_t, std::greater<uint64_t>> store_list_{};
};

} // namespace tinyarmsim::uarch
