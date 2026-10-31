#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include "tinyarmsim/disassembler.hpp"

namespace tinyarmsim {

struct RegWriteRecord {
    uint8_t reg{0};
    uint32_t val{0};
};

struct MemAccessRecord {
    bool is_write{false};
    uint32_t addr{0};
    uint8_t size{4}; // 1, 2, 4 bytes
    uint32_t val{0};
};

struct TraceRecord {
    uint32_t core_id{0};
    uint32_t pc{0};
    uint32_t raw_hex{0};
    uint8_t instr_size{2};
    std::string disasm;
    std::vector<RegWriteRecord> reg_writes;
    std::vector<MemAccessRecord> mem_accesses;
    bool flag_n{false};
    bool flag_z{false};
    bool flag_c{false};
    bool flag_v{false};

    void clear() {
        reg_writes.clear();
        mem_accesses.clear();
    }

    [[nodiscard]] std::string format() const {
        std::ostringstream oss;
        // 1. Core ID & PC
        oss << "core " << core_id << ": 0x" << std::hex << std::setw(8) << std::setfill('0') << pc
            << " (0x" << std::setw(instr_size == 2 ? 4 : 8) << raw_hex << ")" << std::dec;
        
        // 2. Disassembly column (padded)
        oss << "  " << std::left << std::setw(24) << std::setfill(' ') << disasm << std::right;

        // 3. Register writeback
        if (!reg_writes.empty()) {
            oss << " | ";
            for (size_t i = 0; i < reg_writes.size(); ++i) {
                if (i > 0) oss << ", ";
                uint8_t r = reg_writes[i].reg;
                std::string r_name = Disassembler::reg_name(r);
                oss << r_name << " 0x" << std::hex << std::setw(8) << std::setfill('0') << reg_writes[i].val << std::dec;
            }
        }

        // 4. Memory accesses
        if (!mem_accesses.empty()) {
            oss << " | ";
            for (size_t i = 0; i < mem_accesses.size(); ++i) {
                if (i > 0) oss << ", ";
                const auto& mem = mem_accesses[i];
                if (mem.is_write) {
                    oss << "mem[0x" << std::hex << mem.addr << "] <= 0x" << mem.val << std::dec << " (" << static_cast<int>(mem.size) << "B)";
                } else {
                    oss << "mem[0x" << std::hex << mem.addr << "] => 0x" << mem.val << std::dec << " (" << static_cast<int>(mem.size) << "B)";
                }
            }
        }

        // 5. NZCV flags
        oss << " | NZCV=[" << (flag_n ? '1' : '0')
            << (flag_z ? '1' : '0')
            << (flag_c ? '1' : '0')
            << (flag_v ? '1' : '0') << "]";

        return oss.str();
    }
};

} // namespace tinyarmsim
