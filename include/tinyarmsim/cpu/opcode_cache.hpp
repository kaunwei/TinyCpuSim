#pragma once

#include <vector>
#include <array>
#include <unordered_map>
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/decoder.hpp"

namespace tinyarmsim {

class OpcodeCache {
public:
    OpcodeCache() {
        valid_16_.fill(false);
    }

    [[nodiscard]] const DecodedInstruction& get_16(uint16_t raw_instr) {
        if (!valid_16_[raw_instr]) {
            table_16_[raw_instr] = Decoder::decode16(raw_instr);
            valid_16_[raw_instr] = true;
        }
        return table_16_[raw_instr];
    }

    [[nodiscard]] const DecodedInstruction& get_32(uint16_t w1, uint16_t w2) {
        uint32_t key = (static_cast<uint32_t>(w1) << 16) | w2;
        auto it = map_32_.find(key);
        if (it != map_32_.end()) {
            return it->second;
        }
        DecodedInstruction instr = Decoder::decode32(w1, w2);
        auto res = map_32_.emplace(key, std::move(instr));
        return res.first->second;
    }

    void clear() {
        valid_16_.fill(false);
        map_32_.clear();
    }

private:
    std::array<bool, 65536> valid_16_{};
    std::array<DecodedInstruction, 65536> table_16_{};
    std::unordered_map<uint32_t, DecodedInstruction> map_32_;
};

} // namespace tinyarmsim
