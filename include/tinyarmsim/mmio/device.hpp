#pragma once

#include <cstdint>
#include <string>

namespace tinyarmsim::mmio {

enum class AccessSize : uint8_t {
    Byte = 1,
    HalfWord = 2,
    Word = 4
};

class MMIODevice {
public:
    virtual ~MMIODevice() = default;

    [[nodiscard]] virtual uint32_t base_address() const noexcept = 0;
    [[nodiscard]] virtual uint32_t address_span() const noexcept = 0;

    [[nodiscard]] virtual uint32_t read(uint32_t offset, AccessSize size) = 0;
    virtual void write(uint32_t offset, uint32_t value, AccessSize size) = 0;

    virtual void tick([[maybe_unused]] uint64_t cycles) {}
    virtual void reset() {}

    [[nodiscard]] virtual bool has_pending_irq() const noexcept { return false; }
    [[nodiscard]] virtual uint32_t irq_line() const noexcept { return 0; }
    [[nodiscard]] virtual std::string device_name() const { return "MMIODevice"; }
};

} // namespace tinyarmsim::mmio
