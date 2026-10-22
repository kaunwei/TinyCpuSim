# MMIO Timing & Performance Micro-Architecture Code Slices

## 1. MMIO Device Base Interface
`include/tinyarmsim/mmio/device.hpp#L14-L30`:
```cpp
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
```

---

## 2. Timer Device Implementation
`include/tinyarmsim/mmio/timer.hpp#L44-L60`:
```cpp
    void tick(uint64_t cycles) override {
        if ((control_ & 0x01u) == 0) return; // Not enabled
        if (load_ == 0) return;

        if (cycles >= value_) {
            raw_irq_ = true;
            if ((control_ & 0x02u) != 0) { // Periodic auto-reload
                uint64_t rem = (cycles - value_) % load_;
                value_ = static_cast<uint32_t>(load_ - rem);
            } else {
                value_ = 0;
                control_ &= ~0x01u; // One-shot disables timer
            }
        } else {
            value_ -= static_cast<uint32_t>(cycles);
        }
    }
```

---

## 3. UART Immediate Status & Transmission
`include/tinyarmsim/mmio/uart.hpp#L31-L55`:
```cpp
        // Offset 0x04: STATUS
        // bit 0: TX_READY (1), bit 1: RX_VALID (!rx_fifo_.empty()), bit 2: TX_EMPTY (1)
        if (offset == 0x04) {
            uint32_t status = 0x01u | 0x04u;
            if (!rx_fifo_.empty()) status |= 0x02u;
            return status;
        }
        // Offset 0x08: CONTROL
        if (offset == 0x08) return control_;
        // Offset 0x0C: BAUD
        if (offset == 0x0C) return baud_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        // Offset 0x00: DATA
        if (offset == 0x00) {
            tx_output_.push_back(static_cast<char>(value & 0xFFu));
        } else if (offset == 0x08) {
            control_ = value;
        } else if (offset == 0x0C) {
            baud_ = value;
        }
    }
```

---

## 4. SPI Immediate Transfer
`include/tinyarmsim/mmio/spi.hpp#L57-L80`:
```cpp
    void transfer_byte(uint8_t byte) {
        if (cs_ == 0) { // Active transfer
            if (cmd_state_ == 0) {
                if (byte == 0x9F) { // Read JEDEC ID
                    cmd_state_ = 1;
                    rx_data_ = 0xEF; // Winbond Manufacturer ID
                } else {
                    rx_data_ = byte; // Loopback
                }
            } else if (cmd_state_ == 1) {
                rx_data_ = 0x40; // Memory type
                cmd_state_ = 2;
            } else if (cmd_state_ == 2) {
                rx_data_ = 0x18; // Capacity (128M-bit)
                cmd_state_ = 0;
            } else {
                rx_data_ = 0xFF;
            }
        } else {
            rx_data_ = 0xFF;
        }
        sr_ |= 0x04; // RXNE = 1
        sr_ |= 0x02; // TXE = 1
    }
```

---

## 5. MemoryBus Synchronous Dispatch and Tick Forwarding
`include/tinyarmsim/memory_bus.hpp#L79-L85, L149-L158`:
```cpp
    void tick(uint64_t cycles) {
        for (auto& slot : mmio_devices_) {
            if (slot.dev) {
                slot.dev->tick(cycles);
            }
        }
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
```

---

## 6. IsaInterpreter Execution Loop (Missing Bus Tick)
`include/tinyarmsim/interpreter.hpp#L608-L637, L682-L697`:
```cpp
    void step() {
        if (nvic_) {
            // Poll devices for interrupts
            for (const auto& slot : bus_.mmio_devices()) {
                if (slot.dev && slot.dev->has_pending_irq()) {
                    nvic_->set_pending(slot.dev->irq_line());
                }
            }
            int irq = nvic_->get_highest_pending_irq();
            if (irq >= 0) {
                // Auto-stacking onto MSP (R13)
                // ...
                return;
            }
        }

        uint32_t pc = state_.get_pc();
        // ... Fetch, Decode, Execute without bus_.tick() ...
    }

    uint32_t run(uint64_t max_steps = 1000000) {
        // ...
        for (uint64_t step_count = 0; step_count < max_steps; ++step_count) {
            try {
                step();
            } catch (const CpuFaultException& e) {
                // ...
            }
        }
        // ...
    }
```

---

## 7. OOOCore Pipeline Advance (Missing Bus Tick)
`include/tinyarmsim/uarch/ooo_core.hpp#L173-L218`:
```cpp
    // Advance core by 1 clock cycle using Reverse Stage Traversal
    void tick() {
        if (halted_) return;
        cycles_++;
        lsu_.tick_cycle(cycles_);
        if (debug_) std::cout << "=== CYCLE " << cycles_ << " ===" << std::endl;

        // Stage 8: Commit / Retire
        stage_commit();
        // Stage 7: Writeback & Wakeup
        stage_writeback();
        // Stage 6: Execute (Multi-port ALUs, AGUs, Branches)
        stage_execute();
        // Stage 5: Issue / Select from Issue Queue
        stage_issue();
        // Stage 4: Dispatch & ROB / LSU Allocation
        stage_dispatch();
        // Stage 3: Rename & RAT / Checkpointing
        stage_rename();
        // Stage 1 & 2: Fetch & Pre-decode
        if (!config_.is_lsd_enabled() || !lsd_.is_streaming()) {
            fetch_unit_.tick();
        }
        if (fetch_unit_.is_halted() && rob_.is_empty() && rename_queue_.empty() && (!config_.is_lsd_enabled() || !lsd_.is_streaming())) {
            halted_ = true;
        }
    }
```

---

## 8. Baremetal Firmware Test Topology
`tests/baremetal_firmware_test.cpp#L16-L45`:
```cpp
TEST(BaremetalFirmwareTest, FullPeripheralSystemExecution) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto uart = std::make_unique<mmio::UartDevice>();
    auto timer = std::make_unique<mmio::TimerDevice>();
    auto i2c = std::make_unique<mmio::I2CDevice>();
    auto spi = std::make_unique<mmio::SpiDevice>();
    auto crc = std::make_unique<mmio::Crc32Device>();
    auto custom = std::make_unique<mmio::CustomDevice>();
    auto nvic = std::make_unique<mmio::NVICDevice>();

    // ...
    bus.register_device(std::move(uart));
    bus.register_device(std::move(timer));
    bus.register_device(std::move(i2c));
    bus.register_device(std::move(spi));
    bus.register_device(std::move(crc));
    bus.register_device(std::move(custom));
    bus.register_device(std::move(nvic));

    interp.attach_nvic(nvic_ptr);
    // (bus.tick() is never called in this test)
```
