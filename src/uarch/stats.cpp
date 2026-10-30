#include "tinyarmsim/uarch/stats.hpp"

namespace tinyarmsim::uarch {

std::string format_counter_delta(uint64_t current, uint64_t previous) {
    if (current >= previous) {
        return "+" + std::to_string(current - previous);
    }
    return "-" + std::to_string(previous - current);
}

} // namespace tinyarmsim::uarch
