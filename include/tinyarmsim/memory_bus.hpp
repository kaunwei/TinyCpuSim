#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "tinyarmsim/faults.hpp"
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim {

class MemoryBus {
public:
    struct MMIOSlot {
        uint32_t base{0};
        uint32_t span{0};
        std::unique_ptr<mmio::MMIODevice> dev{nullptr};
    };

    explicit MemoryBus(size_t size_bytes = 64 * 1024 * 1024)
        : size_(size_bytes), memory_(size_bytes, 0) {}

    MemoryBus(const MemoryBus&) = delete;
    MemoryBus& operator=(const MemoryBus&) = delete;
    MemoryBus(MemoryBus&&) noexcept = default;
    MemoryBus& operator=(MemoryBus&&) noexcept = default;

    [[nodiscard]] size_t size() const noexcept { return size_; }

    void register_device(uint32_t base, uint32_t span, std::unique_ptr<mmio::MMIODevice> dev) {
        if (!dev) {
            throw std::invalid_argument("Cannot register null MMIODevice");
        }
        if (span == 0) {
            throw std::invalid_argument("MMIODevice address span must be greater than zero");
        }
        uint64_t end = static_cast<uint64_t>(base) + span;
        if (end > 0x100000000ULL) {
            throw std::invalid_argument("MMIODevice address range exceeds 32-bit address space");
        }
        for (const auto& slot : mmio_devices_) {
            uint64_t existing_end = static_cast<uint64_t>(slot.base) + slot.span;
            if (static_cast<uint64_t>(base) < existing_end && end > static_cast<uint64_t>(slot.base)) {
                throw std::invalid_argument("Overlapping MMIO device region registered");
            }
        }
        mmio_devices_.push_back(MMIOSlot{base, span, std::move(dev)});
    }

    void register_device(std::unique_ptr<mmio::MMIODevice> dev) {
        if (!dev) {
            throw std::invalid_argument("Cannot register null MMIODevice");
        }
        uint32_t base = dev->base_address();
        uint32_t span = dev->address_span();
        register_device(base, span, std::move(dev));
    }

    [[nodiscard]] const std::vector<MMIOSlot>& mmio_devices() const noexcept {
        return mmio_devices_;
    }

    [[nodiscard]] std::vector<MMIOSlot>& mmio_devices() noexcept {
        return mmio_devices_;
    }

    [[nodiscard]] mmio::MMIODevice* get_device_at(uint32_t address) const noexcept {
        const auto* slot = find_slot(address);
        return slot ? slot->dev.get() : nullptr;
    }

    template <typename T>
    [[nodiscard]] T* get_device_at(uint32_t address) const noexcept {
        return dynamic_cast<T*>(get_device_at(address));
    }

    void tick(uint64_t cycles) {
        for (auto& slot : mmio_devices_) {
            if (slot.dev) {
                slot.dev->tick(cycles);
            }
        }
    }

    void reset() {
        for (auto& slot : mmio_devices_) {
            if (slot.dev) {
                slot.dev->reset();
            }
        }
    }

    [[nodiscard]] uint8_t read8(uint32_t address) const {
        if (const auto* slot = find_slot(address)) {
            if (static_cast<uint64_t>(address) + 1 > static_cast<uint64_t>(slot->base) + slot->span) {
                throw MemoryFaultException(FaultType::MemoryOutOfBounds, address, "MMIO read out of bounds");
            }
            uint32_t offset = address - slot->base;
            return static_cast<uint8_t>(slot->dev->read(offset, mmio::AccessSize::Byte) & 0xFFu);
        }
        check_bounds(address, 1);
        return memory_[address];
    }

    void write8(uint32_t address, uint8_t value) {
        if (const auto* slot = find_slot(address)) {
            if (static_cast<uint64_t>(address) + 1 > static_cast<uint64_t>(slot->base) + slot->span) {
                throw MemoryFaultException(FaultType::MemoryOutOfBounds, address, "MMIO write out of bounds");
            }
            uint32_t offset = address - slot->base;
            slot->dev->write(offset, static_cast<uint32_t>(value), mmio::AccessSize::Byte);
            return;
        }
        check_bounds(address, 1);
        memory_[address] = value;
    }

    [[nodiscard]] uint16_t read16(uint32_t address) const {
        check_alignment(address, 2);
        if (const auto* slot = find_slot(address)) {
            if (static_cast<uint64_t>(address) + 2 > static_cast<uint64_t>(slot->base) + slot->span) {
                throw MemoryFaultException(FaultType::MemoryOutOfBounds, address, "MMIO read out of bounds");
            }
            uint32_t offset = address - slot->base;
            return static_cast<uint16_t>(slot->dev->read(offset, mmio::AccessSize::HalfWord) & 0xFFFFu);
        }
        check_bounds(address, 2);
        uint16_t val;
        std::memcpy(&val, &memory_[address], sizeof(val));
        return val;
    }

    void write16(uint32_t address, uint16_t value) {
        check_alignment(address, 2);
        if (const auto* slot = find_slot(address)) {
            if (static_cast<uint64_t>(address) + 2 > static_cast<uint64_t>(slot->base) + slot->span) {
                throw MemoryFaultException(FaultType::MemoryOutOfBounds, address, "MMIO write out of bounds");
            }
            uint32_t offset = address - slot->base;
            slot->dev->write(offset, static_cast<uint32_t>(value), mmio::AccessSize::HalfWord);
            return;
        }
        check_bounds(address, 2);
        std::memcpy(&memory_[address], &value, sizeof(value));
    }

    [[nodiscard]] uint32_t read32(uint32_t address) const {
        check_alignment(address, 4);
        if (const auto* slot = find_slot(address)) {
            if (static_cast<uint64_t>(address) + 4 > static_cast<uint64_t>(slot->base) + slot->span) {
                throw MemoryFaultException(FaultType::MemoryOutOfBounds, address, "MMIO read out of bounds");
            }
            uint32_t offset = address - slot->base;
            return slot->dev->read(offset, mmio::AccessSize::Word);
        }
        check_bounds(address, 4);
        uint32_t val;
        std::memcpy(&val, &memory_[address], sizeof(val));
        return val;
    }

    void write32(uint32_t address, uint32_t value) {
        check_alignment(address, 4);
        if (const auto* slot = find_slot(address)) {
            if (static_cast<uint64_t>(address) + 4 > static_cast<uint64_t>(slot->base) + slot->span) {
                throw MemoryFaultException(FaultType::MemoryOutOfBounds, address, "MMIO write out of bounds");
            }
            uint32_t offset = address - slot->base;
            slot->dev->write(offset, value, mmio::AccessSize::Word);
            return;
        }
        check_bounds(address, 4);
        std::memcpy(&memory_[address], &value, sizeof(value));
    }

    [[nodiscard]] uint8_t* get_raw_ptr(uint32_t address, size_t length) {
        check_bounds(address, length);
        return &memory_[address];
    }

    [[nodiscard]] const uint8_t* get_raw_ptr(uint32_t address, size_t length) const {
        check_bounds(address, length);
        return &memory_[address];
    }

private:
    [[nodiscard]] const MMIOSlot* find_slot(uint32_t address) const noexcept {
        for (const auto& slot : mmio_devices_) {
            if (slot.dev && address >= slot.base &&
                static_cast<uint64_t>(address) < static_cast<uint64_t>(slot.base) + slot.span) {
                return &slot;
            }
        }
        return nullptr;
    }

    void check_bounds(uint32_t address, size_t length) const {
        if (static_cast<uint64_t>(address) + length > size_) {
            throw MemoryFaultException(FaultType::MemoryOutOfBounds, address, "Memory access out of bounds");
        }
    }

    void check_alignment(uint32_t address, uint32_t align_bytes) const {
        if ((address % align_bytes) != 0) {
            throw MemoryFaultException(FaultType::MemoryUnaligned, address, "Unaligned memory access");
        }
    }

    size_t size_;
    std::vector<uint8_t> memory_;
    std::vector<MMIOSlot> mmio_devices_;
};

} // namespace tinyarmsim
