#pragma once

#include <memory>
#include <string>
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/platform/virtual_platform.hpp"
#include "tinyarmsim/platform/accelerator_subsystem.hpp"
#include "tinyarmsim/mmio/uart.hpp"
#include "tinyarmsim/mmio/timer.hpp"

namespace tinyarmsim::platform {

class AccelMailboxProxy : public mmio::MMIODevice {
public:
    explicit AccelMailboxProxy(mmio::MMIODevice& target) : target_(target) {}
    [[nodiscard]] uint32_t base_address() const noexcept override { return target_.base_address(); }
    [[nodiscard]] uint32_t address_span() const noexcept override { return target_.address_span(); }
    [[nodiscard]] std::string device_name() const override { return target_.device_name(); }
    [[nodiscard]] bool has_pending_irq() const noexcept override { return false; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return target_.irq_line(); }
    [[nodiscard]] uint32_t read(uint32_t offset, mmio::AccessSize size) override { return target_.read(offset, size); }
    void write(uint32_t offset, uint32_t value, mmio::AccessSize size) override { target_.write(offset, value, size); }
    void tick(uint64_t cycles) override { target_.tick(cycles); }
    void reset() override { target_.reset(); }
private:
    mmio::MMIODevice& target_;
};

class McuConvPlatform {
public:
    static constexpr size_t HOST_MEM_SIZE = 64 * 1024 * 1024;
    static constexpr uint32_t ACCEL_BASE = 0x50000000;
    static constexpr uint32_t ACCEL_HOST_IRQ = 16;
    static constexpr uint32_t FIRMWARE_VTOR = 0x00010000;

    McuConvPlatform()
        : platform_(),
          host_bus_(platform_.create_bus("host", HOST_MEM_SIZE)),
          host_cpu_(platform_.create_cpu("host_cpu", host_bus_)),
          accel_subsystem_() {
        
        // Register NVIC MMIO devices on their respective buses
        host_bus_.register_device(std::make_unique<ProxyMMIODevice>(host_cpu_.nvic, 0xE000E000, 0x1000));
        accel_subsystem_.accel_bus().register_device(
            std::make_unique<ProxyMMIODevice>(accel_subsystem_.accel_nvic(), 0xE000E000, 0x1000));

        // Attach standard Host peripherals (UartDevice at 0x40000000, TimerDevice at 0x40001000)
        host_bus_.register_device(std::make_unique<mmio::UartDevice>(0x40000000, 0x1000));
        host_bus_.register_device(std::make_unique<mmio::TimerDevice>(0x40001000, 0x1000));

        // Attach Accelerator subsystem to host bus at 0x50000000
        accel_subsystem_.attach_to_host_bus(host_bus_, ACCEL_BASE, ACCEL_HOST_IRQ);

        // Ensure accelerator private bus does not self-trigger on Host doorbell interrupt
        for (auto& slot : accel_subsystem_.accel_bus().mmio_devices()) {
            if (slot.base == ACCEL_BASE) {
                slot.dev = std::make_unique<AccelMailboxProxy>(accel_subsystem_.mailbox());
            }
        }
    }

    [[nodiscard]] VirtualPlatform& platform() noexcept { return platform_; }
    [[nodiscard]] MemoryBus& host_bus() noexcept { return host_bus_; }
    [[nodiscard]] CpuInstance& host_cpu() noexcept { return host_cpu_; }
    [[nodiscard]] ConvolutionAcceleratorSubsystem& accelerator() noexcept { return accel_subsystem_; }

    void load_host_firmware(const std::string& elf_path) {
        platform_.load_elf("host_cpu", elf_path, Loader::ElfLoadMode::Firmware);
        host_cpu_.nvic.set_vtor(FIRMWARE_VTOR);
        Loader::boot_cortex_m(host_bus_, host_cpu_.state, FIRMWARE_VTOR);
    }

    void load_accel_firmware(const std::string& elf_path) {
        accel_subsystem_.load_firmware(elf_path, Loader::ElfLoadMode::Firmware);
        accel_subsystem_.accel_nvic().set_vtor(FIRMWARE_VTOR);
        accel_subsystem_.boot_cortex_m(FIRMWARE_VTOR);
    }

    bool step_quantum(uint64_t quantum_size = 500) {
        bool host_active = false;
        if (!host_cpu_.halted) {
            uint64_t executed = 0;
            while (executed < quantum_size && !host_cpu_.halted) {
                try {
                    host_cpu_.interpreter.step();
                    executed++;
                } catch (const CpuFaultException&) {
                    host_cpu_.halted = true;
                    break;
                }
            }
            if (!host_cpu_.halted) {
                host_active = true;
            }
        }

        bool accel_active = false;
        if (!accel_subsystem_.is_halted()) {
            accel_subsystem_.step_quantum(quantum_size);
            if (!accel_subsystem_.is_halted()) {
                accel_active = true;
            }
        }

        host_bus_.tick(1);
        return host_active || accel_active;
    }

    uint64_t run(uint64_t max_cycles, uint64_t quantum_size = 500) {
        uint64_t total_steps = 0;
        while (total_steps < max_cycles) {
            bool active = step_quantum(quantum_size);
            total_steps += quantum_size * 2;
            if (!active) {
                break;
            }
        }
        return total_steps;
    }

private:
    VirtualPlatform platform_;
    MemoryBus& host_bus_;
    CpuInstance& host_cpu_;
    ConvolutionAcceleratorSubsystem accel_subsystem_;
};

} // namespace tinyarmsim::platform
