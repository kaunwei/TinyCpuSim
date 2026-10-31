#pragma once

#include <memory>
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/uarch/config.hpp"

namespace tinyarmsim::platform {

class BasicPerfPlatform {
public:
    explicit BasicPerfPlatform(const uarch::CoreConfig& config = uarch::CoreConfig{},
                               size_t mem_size = 64 * 1024 * 1024)
        : bus_(mem_size), config_(config) {}

    [[nodiscard]] MemoryBus& bus() noexcept { return bus_; }
    [[nodiscard]] const MemoryBus& bus() const noexcept { return bus_; }
    [[nodiscard]] const uarch::CoreConfig& config() const noexcept { return config_; }

private:
    MemoryBus bus_;
    uarch::CoreConfig config_;
};

} // namespace tinyarmsim::platform
