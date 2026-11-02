#include "tinyarmsim/cpu/state.hpp"
#include "tinyarmsim/cpu/instruction.hpp"

namespace tinyarmsim {

const char* condition_code_to_string(ConditionCode cond) noexcept {
    switch (cond) {
        case ConditionCode::EQ: return "EQ";
        case ConditionCode::NE: return "NE";
        case ConditionCode::CS: return "CS";
        case ConditionCode::CC: return "CC";
        case ConditionCode::MI: return "MI";
        case ConditionCode::PL: return "PL";
        case ConditionCode::VS: return "VS";
        case ConditionCode::VC: return "VC";
        case ConditionCode::HI: return "HI";
        case ConditionCode::LS: return "LS";
        case ConditionCode::GE: return "GE";
        case ConditionCode::LT: return "LT";
        case ConditionCode::GT: return "GT";
        case ConditionCode::LE: return "LE";
        case ConditionCode::AL: return "AL";
        case ConditionCode::NV: return "NV";
        default: return "??";
    }
}

} // namespace tinyarmsim
