#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <fstream>
#include <stdexcept>

#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/loader.hpp"
#include "tinyarmsim/mmio/nvic.hpp"
#include "tinyarmsim/mmio/mailbox_scratchpad.hpp"

namespace tinyarmsim::platform {

class ProxyMMIODevice : public mmio::MMIODevice {
public:
    ProxyMMIODevice(mmio::MMIODevice& target, uint32_t base, uint32_t span)
        : target_(target), base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return target_.device_name(); }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return target_.has_pending_irq();
    }

    [[nodiscard]] uint32_t irq_line() const noexcept override {
        return target_.irq_line();
    }

    [[nodiscard]] uint32_t read(uint32_t offset, mmio::AccessSize size) override {
        return target_.read(offset, size);
    }

    void write(uint32_t offset, uint32_t value, mmio::AccessSize size) override {
        target_.write(offset, value, size);
    }

    void tick(uint64_t cycles) override {
        target_.tick(cycles);
    }

    void reset() override {
        target_.reset();
    }

private:
    mmio::MMIODevice& target_;
    uint32_t base_;
    uint32_t span_;
};

class ConvolutionAcceleratorSubsystem {
public:
    static constexpr size_t DEFAULT_ACCEL_MEM_SIZE = 16 * 1024 * 1024; // 16MB private bus
    static constexpr uint32_t DEFAULT_ACCEL_IRQ_LINE = 4;

    explicit ConvolutionAcceleratorSubsystem(size_t accel_mem_size = DEFAULT_ACCEL_MEM_SIZE,
                                             uint32_t accel_irq_line = DEFAULT_ACCEL_IRQ_LINE)
        : accel_bus_(accel_mem_size),
          accel_state_(),
          accel_nvic_(),
          accel_cpu_(accel_state_, accel_bus_),
          mailbox_(),
          accel_irq_line_(accel_irq_line) {
        accel_cpu_.attach_nvic(&accel_nvic_);

        // When host rings doorbell to accel, assert interrupt on accelerator's local NVIC
        mailbox_.set_accel_irq_callback([this]() {
            accel_nvic_.set_pending(accel_irq_line_);
        });

        // Register proxy on accelerator's private bus so accelerator can access CSRs & scratchpad
        accel_bus_.register_device(std::make_unique<ProxyMMIODevice>(
            mailbox_, mailbox_.base_address(), mailbox_.address_span()));
    }

    void attach_to_host_bus(MemoryBus& host_bus, uint32_t base_address = mmio::MailboxScratchpadDevice::DEFAULT_BASE,
                            uint32_t host_irq = mmio::MailboxScratchpadDevice::DEFAULT_HOST_IRQ_LINE) {
        (void)host_irq;
        host_bus.register_device(std::make_unique<ProxyMMIODevice>(
            mailbox_, base_address, mailbox_.address_span()));
    }

    void load_firmware(const std::string& elf_path, Loader::ElfLoadMode mode = Loader::ElfLoadMode::Firmware) {
        std::ifstream stream(elf_path, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("Failed to open firmware ELF: " + elf_path);
        }
        Loader::load_elf(stream, accel_bus_, accel_state_, mode);
    }

    void boot_cortex_m(uint32_t vtor_base = 0x00000000) {
        Loader::boot_cortex_m(accel_bus_, accel_state_, vtor_base);
    }

    void step_quantum(uint64_t instructions) {
        if (halted_) return;
        uint64_t executed = 0;
        while (executed < instructions && !halted_) {
            try {
                accel_cpu_.step();
                executed++;
            } catch (const CpuFaultException&) {
                halted_ = true;
                break;
            } catch (...) {
                halted_ = true;
                throw;
            }
        }
        accel_bus_.tick(1);
    }

    [[nodiscard]] MemoryBus& accel_bus() noexcept { return accel_bus_; }
    [[nodiscard]] const MemoryBus& accel_bus() const noexcept { return accel_bus_; }
    [[nodiscard]] ArchitecturalState& accel_state() noexcept { return accel_state_; }
    [[nodiscard]] const ArchitecturalState& accel_state() const noexcept { return accel_state_; }
    [[nodiscard]] mmio::NVICDevice& accel_nvic() noexcept { return accel_nvic_; }
    [[nodiscard]] mmio::MailboxScratchpadDevice& mailbox() noexcept { return mailbox_; }
    [[nodiscard]] bool is_halted() const noexcept { return halted_; }
    void set_halted(bool halted) noexcept { halted_ = halted; }

private:
    MemoryBus accel_bus_;
    ArchitecturalState accel_state_;
    mmio::NVICDevice accel_nvic_;
    IsaInterpreter accel_cpu_;
    mmio::MailboxScratchpadDevice mailbox_;
    uint32_t accel_irq_line_{DEFAULT_ACCEL_IRQ_LINE};
    bool halted_{false};
};

} // namespace tinyarmsim::platform
