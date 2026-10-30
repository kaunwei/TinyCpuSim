#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tinyarmsim {

constexpr uint32_t VERSION_MAJOR = 0;
constexpr uint32_t VERSION_MINOR = 1;
constexpr uint32_t VERSION_PATCH = 0;

[[nodiscard]] constexpr std::string_view get_version_string() noexcept {
    return "0.1.0";
}

[[nodiscard]] const char* get_build_profile() noexcept;

[[nodiscard]] inline uint32_t popcount(uint32_t val) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_popcount(val));
#else
    uint32_t count = 0;
    while (val) {
        val &= (val - 1);
        count++;
    }
    return count;
#endif
}

} // namespace tinyarmsim
