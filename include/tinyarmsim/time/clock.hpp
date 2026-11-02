#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace tinyarmsim::time {

class ClockConversion {
public:
    ClockConversion(uint64_t numerator, uint64_t denominator)
        : numerator_(numerator), denominator_(denominator) {
        if (!numerator || !denominator) {
            throw std::invalid_argument("zero clock ratio");
        }
    }

    uint64_t convert(uint64_t input) {
        constexpr auto max = std::numeric_limits<uint64_t>::max();
        if (input > (max - remainder_) / numerator_) {
            throw std::overflow_error("clock conversion overflow");
        }
        const auto total = input * numerator_ + remainder_;
        remainder_ = total % denominator_;
        return total / denominator_;
    }

    void reset() noexcept { remainder_ = 0; }

private:
    uint64_t numerator_, denominator_, remainder_ = 0;
};

} // namespace tinyarmsim::time
