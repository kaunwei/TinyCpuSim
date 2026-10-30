#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace tinyarmsim {

enum class FaultType {
    MemoryOutOfBounds,
    MemoryUnaligned,
    UndefinedInstruction,
    IllegalRegister,
    SoftwareInterrupt
};

class CpuFaultException : public std::runtime_error {
public:
    CpuFaultException(FaultType type, const std::string& message)
        : std::runtime_error(message), fault_type_(type) {}

    [[nodiscard]] FaultType get_fault_type() const noexcept { return fault_type_; }

private:
    FaultType fault_type_;
};

class MemoryFaultException : public CpuFaultException {
public:
    MemoryFaultException(FaultType type, uint32_t address, const std::string& msg)
        : CpuFaultException(type, msg + " at address: 0x" + to_hex(address)), address_(address) {}

    [[nodiscard]] uint32_t get_address() const noexcept { return address_; }

private:
    static std::string to_hex(uint32_t val) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", val);
        return std::string(buf);
    }
    uint32_t address_;
};

class UndefinedInstructionException : public CpuFaultException {
public:
    UndefinedInstructionException(uint32_t raw_instruction, uint32_t pc)
        : CpuFaultException(FaultType::UndefinedInstruction,
                            "Undefined instruction 0x" + to_hex(raw_instruction) + " at PC 0x" + to_hex(pc)),
          raw_instruction_(raw_instruction), pc_(pc) {}

    [[nodiscard]] uint32_t get_raw_instruction() const noexcept { return raw_instruction_; }
    [[nodiscard]] uint32_t get_pc() const noexcept { return pc_; }

private:
    static std::string to_hex(uint32_t val) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", val);
        return std::string(buf);
    }
    uint32_t raw_instruction_;
    uint32_t pc_;
};

} // namespace tinyarmsim

namespace tinyarmsim {
const char* fault_type_to_string(FaultType type) noexcept;
} // namespace tinyarmsim
