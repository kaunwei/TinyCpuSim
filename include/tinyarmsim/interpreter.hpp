#pragma once

#include <cstdint>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <fstream>
#include "tinyarmsim/common.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/decoder.hpp"
#include "tinyarmsim/opcode_cache.hpp"
#include "tinyarmsim/trace.hpp"
#include "tinyarmsim/faults.hpp"
#include "tinyarmsim/uarch/topdown_profiler.hpp"
#include "tinyarmsim/uarch/slice_manager.hpp"

namespace tinyarmsim {

struct SimulationStats {
    uint64_t instruction_count{0};
    double elapsed_seconds{0.0};
    double instructions_per_second{0.0};
    std::unordered_map<Opcode, uint64_t> opcode_counts;
};

class IsaInterpreter {
public:
    IsaInterpreter(ArchitecturalState& state, MemoryBus& bus) noexcept
        : state_(state), bus_(bus) {}

    void set_profiler(uarch::TopDownProfiler* profiler) noexcept {
        profiler_ = profiler;
    }

    void set_slice_manager(uarch::SliceManager* sm) noexcept {
        slice_manager_ = sm;
    }

    [[nodiscard]] uarch::SliceManager* get_slice_manager() const noexcept {
        return slice_manager_;
    }

    void set_logging(bool enable) noexcept {
        logging_enabled_ = enable;
    }

    [[nodiscard]] bool is_logging_enabled() const noexcept {
        return logging_enabled_;
    }

    [[nodiscard]] const SimulationStats& get_stats() const noexcept {
        return stats_;
    }

    void reset_stats() noexcept {
        stats_ = SimulationStats{};
    }

    [[nodiscard]] bool evaluate_condition(ConditionCode cond) const noexcept {
        bool n = state_.get_flag_n();
        bool z = state_.get_flag_z();
        bool c = state_.get_flag_c();
        bool v = state_.get_flag_v();

        switch (cond) {
            case ConditionCode::EQ: return z;
            case ConditionCode::NE: return !z;
            case ConditionCode::CS: return c;
            case ConditionCode::CC: return !c;
            case ConditionCode::MI: return n;
            case ConditionCode::PL: return !n;
            case ConditionCode::VS: return v;
            case ConditionCode::VC: return !v;
            case ConditionCode::HI: return c && !z;
            case ConditionCode::LS: return !c || z;
            case ConditionCode::GE: return n == v;
            case ConditionCode::LT: return n != v;
            case ConditionCode::GT: return !z && (n == v);
            case ConditionCode::LE: return z || (n != v);
            case ConditionCode::AL: return true;
            case ConditionCode::NV: return false;
        }
        return false;
    }

    void execute(const DecodedInstruction& instr) {
        uint32_t current_pc = state_.get_pc();
        stats_.opcode_counts[instr.op]++;
        stats_.instruction_count++;

        if (logging_enabled_) {
            trace_.clear();
            trace_.core_id = 0;
            trace_.pc = current_pc;
            trace_.raw_hex = instr.raw_hex;
            trace_.instr_size = instr.instr_size;
            trace_.disasm = instr.disasm.empty() ? std::string(opcode_to_string(instr.op)) : instr.disasm;
        }

        if (!evaluate_condition(instr.cond)) {
            state_.advance_pc(instr.instr_size);
            log_trace("[condition false]");
            return;
        }

        bool pc_written = false;

        auto write_reg = [&](size_t r, uint32_t val) {
            state_.set_reg(r, val);
            if (logging_enabled_) {
                trace_.reg_writes.push_back({static_cast<uint8_t>(r), val});
            }
            if (r == REG_PC) pc_written = true;
        };

        auto write_sp = [&](uint32_t val) {
            write_reg(REG_SP, val);
        };

        auto write_lr = [&](uint32_t val) {
            write_reg(REG_LR, val);
        };

        auto write_pc = [&](uint32_t val) {
            write_reg(REG_PC, val);
        };

        auto write_mem = [&](uint32_t addr, uint32_t val, uint8_t size) {
            if (size == 1) bus_.write8(addr, static_cast<uint8_t>(val & 0xFF));
            else if (size == 2) bus_.write16(addr, static_cast<uint16_t>(val & 0xFFFF));
            else if (size == 4) bus_.write32(addr, val);
            if (logging_enabled_) {
                trace_.mem_accesses.push_back({true, addr, size, val});
            }
        };

        auto read_mem = [&](uint32_t addr, uint8_t size, bool is_signed) -> uint32_t {
            uint32_t val = 0;
            if (size == 1) {
                val = is_signed ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(bus_.read8(addr))))
                                : bus_.read8(addr);
            } else if (size == 2) {
                val = is_signed ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(bus_.read16(addr))))
                                : bus_.read16(addr);
            } else if (size == 4) {
                val = bus_.read32(addr);
            }
            if (logging_enabled_) {
                trace_.mem_accesses.push_back({false, addr, size, val});
            }
            return val;
        };

        switch (instr.op) {
            case Opcode::MOV: {
                uint32_t val = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                write_reg(instr.rd, val);
                if (instr.set_flags) {
                    state_.set_nz_flags(val);
                }
                break;
            }

            case Opcode::MVN: {
                uint32_t val = ~(instr.is_imm ? instr.imm : state_.get_reg(instr.rm));
                write_reg(instr.rd, val);
                if (instr.set_flags) {
                    state_.set_nz_flags(val);
                }
                break;
            }

            case Opcode::MOVW: {
                write_reg(instr.rd, instr.imm & 0xFFFFu);
                break;
            }

            case Opcode::MOVT: {
                uint32_t current = state_.get_reg(instr.rd);
                uint32_t updated = (current & 0x0000FFFFu) | ((instr.imm & 0xFFFFu) << 16);
                write_reg(instr.rd, updated);
                break;
            }

            case Opcode::ADD: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint64_t result = static_cast<uint64_t>(op1) + static_cast<uint64_t>(op2);
                uint32_t res32 = static_cast<uint32_t>(result);

                write_reg(instr.rd, res32);

                if (instr.set_flags) {
                    bool n = (res32 & 0x80000000u) != 0;
                    bool z = (res32 == 0);
                    bool c = (result > 0xFFFFFFFFu);
                    bool v = (~(op1 ^ op2) & (op1 ^ res32) & 0x80000000u) != 0;
                    state_.set_flags(n, z, c, v);
                }
                break;
            }

            case Opcode::ADC: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t carry_in = state_.get_flag_c() ? 1u : 0u;
                uint64_t result = static_cast<uint64_t>(op1) + static_cast<uint64_t>(op2) + carry_in;
                uint32_t res32 = static_cast<uint32_t>(result);

                write_reg(instr.rd, res32);

                if (instr.set_flags) {
                    bool n = (res32 & 0x80000000u) != 0;
                    bool z = (res32 == 0);
                    bool c = (result > 0xFFFFFFFFu);
                    bool v = (~(op1 ^ op2) & (op1 ^ res32) & 0x80000000u) != 0;
                    state_.set_flags(n, z, c, v);
                }
                break;
            }

            case Opcode::SUB:
            case Opcode::CMP: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint64_t result = static_cast<uint64_t>(op1) - static_cast<uint64_t>(op2);
                uint32_t res32 = static_cast<uint32_t>(result);

                if (instr.op != Opcode::CMP) {
                    write_reg(instr.rd, res32);
                }

                if (instr.set_flags) {
                    bool n = (res32 & 0x80000000u) != 0;
                    bool z = (res32 == 0);
                    bool c = (op1 >= op2);
                    bool v = ((op1 ^ op2) & (op1 ^ res32) & 0x80000000u) != 0;
                    state_.set_flags(n, z, c, v);
                }
                break;
            }

            case Opcode::SBC: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t borrow = state_.get_flag_c() ? 0u : 1u;
                uint64_t result = static_cast<uint64_t>(op1) - static_cast<uint64_t>(op2) - borrow;
                uint32_t res32 = static_cast<uint32_t>(result);

                write_reg(instr.rd, res32);

                if (instr.set_flags) {
                    bool n = (res32 & 0x80000000u) != 0;
                    bool z = (res32 == 0);
                    bool c = (static_cast<uint64_t>(op1) >= static_cast<uint64_t>(op2) + borrow);
                    bool v = ((op1 ^ op2) & (op1 ^ res32) & 0x80000000u) != 0;
                    state_.set_flags(n, z, c, v);
                }
                break;
            }

            case Opcode::RSB: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint64_t result = static_cast<uint64_t>(op2) - static_cast<uint64_t>(op1);
                uint32_t res32 = static_cast<uint32_t>(result);

                write_reg(instr.rd, res32);

                if (instr.set_flags) {
                    bool n = (res32 & 0x80000000u) != 0;
                    bool z = (res32 == 0);
                    bool c = (op2 >= op1);
                    bool v = ((op2 ^ op1) & (op2 ^ res32) & 0x80000000u) != 0;
                    state_.set_flags(n, z, c, v);
                }
                break;
            }

            case Opcode::MUL: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = state_.get_reg(instr.rm);
                uint32_t res = op1 * op2;
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::MLA: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = state_.get_reg(instr.rm);
                uint32_t op3 = state_.get_reg(instr.rs);
                uint32_t res = (op1 * op2) + op3;
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::AND:
            case Opcode::TST: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t res = op1 & op2;
                if (instr.op != Opcode::TST) {
                    write_reg(instr.rd, res);
                }
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::ORR: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t res = op1 | op2;
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::EOR:
            case Opcode::TEQ: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t res = op1 ^ op2;
                if (instr.op != Opcode::TEQ) {
                    write_reg(instr.rd, res);
                }
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::BIC: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t res = op1 & (~op2);
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::CMN: {
                uint32_t op1 = state_.get_reg(instr.rn);
                uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint64_t result = static_cast<uint64_t>(op1) + static_cast<uint64_t>(op2);
                uint32_t res32 = static_cast<uint32_t>(result);

                if (instr.set_flags) {
                    bool n = (res32 & 0x80000000u) != 0;
                    bool z = (res32 == 0);
                    bool c = (result > 0xFFFFFFFFu);
                    bool v = (~(op1 ^ op2) & (op1 ^ res32) & 0x80000000u) != 0;
                    state_.set_flags(n, z, c, v);
                }
                break;
            }

            case Opcode::LSL: {
                uint32_t val = state_.get_reg(instr.rn);
                uint32_t shift = instr.is_imm ? instr.imm : (state_.get_reg(instr.rm) & 0xFFu);
                uint32_t res = (shift >= 32) ? 0 : (val << shift);
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::LSR: {
                uint32_t val = state_.get_reg(instr.rn);
                uint32_t shift = instr.is_imm ? instr.imm : (state_.get_reg(instr.rm) & 0xFFu);
                uint32_t res = (shift >= 32) ? 0 : (val >> shift);
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::ASR: {
                int32_t val = static_cast<int32_t>(state_.get_reg(instr.rn));
                uint32_t shift = instr.is_imm ? instr.imm : (state_.get_reg(instr.rm) & 0xFFu);
                if (shift >= 32) shift = 31;
                uint32_t res = static_cast<uint32_t>(val >> shift);
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            case Opcode::ROR: {
                uint32_t val = state_.get_reg(instr.rn);
                uint32_t shift = (instr.is_imm ? instr.imm : state_.get_reg(instr.rm)) & 31u;
                uint32_t res = (shift == 0) ? val : ((val >> shift) | (val << (32 - shift)));
                write_reg(instr.rd, res);
                if (instr.set_flags) {
                    state_.set_nz_flags(res);
                }
                break;
            }

            // === Branching Instructions ===
            case Opcode::B: {
                uint32_t target = instr.is_relative ? static_cast<uint32_t>(static_cast<int32_t>(current_pc + 4) + static_cast<int32_t>(instr.imm)) : instr.imm;
                write_pc(target);
                break;
            }

            case Opcode::BL:
            case Opcode::BLX: {
                uint32_t return_address = current_pc + instr.instr_size;
                write_lr(return_address | 1u);
                uint32_t target = 0;
                if (instr.is_imm) {
                    target = instr.is_relative ? static_cast<uint32_t>(static_cast<int32_t>(current_pc + 4) + static_cast<int32_t>(instr.imm)) : instr.imm;
                } else {
                    target = state_.get_reg(instr.rm) & ~1u;
                }
                write_pc(target);
                break;
            }

            case Opcode::BX: {
                uint32_t target = state_.get_reg(instr.rm) & ~1u;
                write_pc(target);
                break;
            }

            case Opcode::CBZ: {
                if (state_.get_reg(instr.rn) == 0) {
                    uint32_t target = instr.is_relative ? (current_pc + 4 + instr.imm) : instr.imm;
                    write_pc(target);
                }
                break;
            }

            case Opcode::CBNZ: {
                if (state_.get_reg(instr.rn) != 0) {
                    uint32_t target = instr.is_relative ? (current_pc + 4 + instr.imm) : instr.imm;
                    write_pc(target);
                }
                break;
            }

            // === Load / Store Single ===
            case Opcode::LDR:
            case Opcode::LDRB:
            case Opcode::LDRH:
            case Opcode::LDRSB:
            case Opcode::LDRSH: {
                uint32_t base = (instr.rn == REG_PC) ? ((current_pc + 4) & ~3u) : state_.get_reg(instr.rn);
                uint32_t offset = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t addr = base + offset;

                uint32_t loaded_val = read_mem(addr, instr.mem_size, instr.is_signed_mem);
                if (instr.rd == REG_PC) {
                    write_pc(loaded_val & ~1u);
                } else {
                    write_reg(instr.rd, loaded_val);
                }
                break;
            }

            case Opcode::STR:
            case Opcode::STRB:
            case Opcode::STRH: {
                uint32_t base = state_.get_reg(instr.rn);
                uint32_t offset = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
                uint32_t addr = base + offset;
                uint32_t val = state_.get_reg(instr.rd);

                write_mem(addr, val, instr.mem_size);
                break;
            }

            // === Stack Operations: PUSH / POP ===
            case Opcode::PUSH: {
                uint32_t sp = state_.get_sp();
                uint32_t reg_count = popcount(instr.register_list);
                uint32_t new_sp = sp - (reg_count * 4);
                uint32_t cur_addr = new_sp;
                for (size_t i = 0; i < NUM_REGISTERS; ++i) {
                    if (instr.register_list & (1 << i)) {
                        write_mem(cur_addr, state_.get_reg(i), 4);
                        cur_addr += 4;
                    }
                }
                write_sp(new_sp);
                break;
            }

            case Opcode::POP: {
                uint32_t sp = state_.get_sp();
                uint32_t cur_addr = sp;
                for (size_t i = 0; i < NUM_REGISTERS; ++i) {
                    if (instr.register_list & (1 << i)) {
                        uint32_t val = read_mem(cur_addr, 4, false);
                        if (i == REG_PC) {
                            write_pc(val & ~1u);
                        } else {
                            write_reg(i, val);
                        }
                        cur_addr += 4;
                    }
                }
                write_sp(cur_addr);
                break;
            }

            // === Multiple Load/Store: LDM / STM ===
            case Opcode::STM: {
                uint32_t base = state_.get_reg(instr.rn);
                uint32_t cur_addr = base;
                for (size_t i = 0; i < NUM_REGISTERS; ++i) {
                    if (instr.register_list & (1 << i)) {
                        write_mem(cur_addr, state_.get_reg(i), 4);
                        cur_addr += 4;
                    }
                }
                if (instr.writeback) {
                    write_reg(instr.rn, cur_addr);
                }
                break;
            }

            case Opcode::LDM: {
                uint32_t base = state_.get_reg(instr.rn);
                uint32_t cur_addr = base;
                for (size_t i = 0; i < NUM_REGISTERS; ++i) {
                    if (instr.register_list & (1 << i)) {
                        uint32_t val = read_mem(cur_addr, 4, false);
                        if (i == REG_PC) {
                            write_pc(val & ~1u);
                        } else {
                            write_reg(i, val);
                        }
                        cur_addr += 4;
                    }
                }
                if (instr.writeback) {
                    write_reg(instr.rn, cur_addr);
                }
                break;
            }

            // === System Control ===
            case Opcode::MRS: {
                write_reg(instr.rd, state_.get_cpsr());
                break;
            }

            case Opcode::MSR: {
                uint32_t val = state_.get_reg(instr.rn);
                state_.set_cpsr((state_.get_cpsr() & 0x0FFFFFFFu) | (val & 0xF0000000u));
                break;
            }

            case Opcode::NOP:
                break;

            case Opcode::SVC:
                log_trace();
                if (instr.imm == 0x50) { // m5_reset_stats
                    if (profiler_) profiler_->trigger_m5op(0x50);
                    break;
                }
                if (instr.imm == 0x51) { // m5_dump_stats
                    if (profiler_) profiler_->trigger_m5op(0x51);
                    break;
                }
                if (instr.imm == 0x52) { // m5_exit
                    if (profiler_) profiler_->trigger_m5op(0x52);
                    throw CpuFaultException(FaultType::SoftwareInterrupt, "m5_exit");
                }
                throw CpuFaultException(FaultType::SoftwareInterrupt, "SVC interrupt with code " + std::to_string(instr.imm));

            default:
                throw CpuFaultException(FaultType::UndefinedInstruction, "Unhandled opcode in interpreter");
        }

        log_trace();

        if (!pc_written) {
            state_.advance_pc(instr.instr_size);
        }
    }

    void step() {
        uint32_t pc = state_.get_pc();
        uint16_t first_halfword = bus_.read16(pc);
        if (Decoder::is_32bit_thumb(first_halfword)) {
            uint16_t second_halfword = bus_.read16(pc + 2);
            const DecodedInstruction& instr = opcode_cache_.get_32(first_halfword, second_halfword);
            execute(instr);
        } else {
            const DecodedInstruction& instr = opcode_cache_.get_16(first_halfword);
            execute(instr);
        }

        check_and_trigger_slice();
    }

    uint32_t run(uint64_t max_steps = 1000000) {
        auto start_time = std::chrono::high_resolution_clock::now();

        auto update_timing = [&]() {
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = end_time - start_time;
            stats_.elapsed_seconds = diff.count();
            if (stats_.elapsed_seconds > 0.0) {
                stats_.instructions_per_second = static_cast<double>(stats_.instruction_count) / stats_.elapsed_seconds;
            }
        };

        for (uint64_t step_count = 0; step_count < max_steps; ++step_count) {
            try {
                step();
            } catch (const CpuFaultException& e) {
                update_timing();
                flush_slice();
                if (e.get_fault_type() == FaultType::SoftwareInterrupt) {
                    return state_.get_reg(0);
                }
                throw;
            } catch (...) {
                update_timing();
                flush_slice();
                throw;
            }
        }
        update_timing();
        flush_slice();
        throw CpuFaultException(FaultType::MemoryOutOfBounds, "Simulation execution exceeded max step limit (" + std::to_string(max_steps) + ")");
    }

    void dump_coverage_csv(std::ostream& out) const {
        out << "Opcode,Name,Count\n";
        for (int i = 1; i <= static_cast<int>(Opcode::NOP); ++i) {
            Opcode op = static_cast<Opcode>(i);
            auto it = stats_.opcode_counts.find(op);
            uint64_t count = (it != stats_.opcode_counts.end()) ? it->second : 0;
            out << i << "," << opcode_to_string(op) << "," << count << "\n";
        }
    }

private:
    void check_and_trigger_slice() {
        if (!slice_manager_ || !slice_manager_->get_config().is_active()) return;

        bool trigger = false;
        if (slice_manager_->get_config().interval_instructions > 0) {
            trigger = slice_manager_->should_trigger_instruction(stats_.instruction_count);
        }
        if (!trigger && slice_manager_->get_config().interval_ticks > 0) {
            trigger = slice_manager_->should_trigger_tick(stats_.instruction_count);
        }

        if (trigger) {
            uarch::UArchStats ustats;
            ustats.total_simulated_cycles = stats_.instruction_count; // In interpreter, 1 cycle per inst
            uarch::CoreStats core;
            core.cycles = stats_.instruction_count;
            core.committed_instructions = stats_.instruction_count;
            core.committed_uops = stats_.instruction_count;
            ustats.cores.push_back(core);
            slice_manager_->capture_slice(ustats);
        }
    }

    void flush_slice() {
        if (!slice_manager_ || !slice_manager_->get_config().is_active()) return;
        uarch::UArchStats ustats;
        ustats.total_simulated_cycles = stats_.instruction_count;
        uarch::CoreStats core;
        core.cycles = stats_.instruction_count;
        core.committed_instructions = stats_.instruction_count;
        core.committed_uops = stats_.instruction_count;
        ustats.cores.push_back(core);
        slice_manager_->flush_final_slice(ustats);
    }

    void log_trace(std::string_view note = "") {
        if (logging_enabled_) {
            trace_.flag_n = state_.get_flag_n();
            trace_.flag_z = state_.get_flag_z();
            trace_.flag_c = state_.get_flag_c();
            trace_.flag_v = state_.get_flag_v();
            if (note.empty()) {
                std::cout << trace_.format() << "\n";
            } else {
                std::cout << trace_.format() << " " << note << "\n";
            }
        }
    }

    ArchitecturalState& state_;
    MemoryBus& bus_;
    SimulationStats stats_{};
    bool logging_enabled_{false};
    OpcodeCache opcode_cache_{};
    TraceRecord trace_{};
    uarch::TopDownProfiler* profiler_{nullptr};
    uarch::SliceManager* slice_manager_{nullptr};
};

} // namespace tinyarmsim
