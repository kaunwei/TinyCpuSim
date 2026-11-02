#include "tinyarmsim/cpu/faults.hpp"

namespace tinyarmsim {

const char* fault_type_to_string(FaultType type) noexcept {
    switch (type) {
        case FaultType::MemoryOutOfBounds: return "MemoryOutOfBounds";
        case FaultType::MemoryUnaligned: return "MemoryUnaligned";
        case FaultType::UndefinedInstruction: return "UndefinedInstruction";
        case FaultType::IllegalRegister: return "IllegalRegister";
        case FaultType::SoftwareInterrupt: return "SoftwareInterrupt";
        default: return "UnknownFault";
    }
}

} // namespace tinyarmsim
