import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwMemoryRam(unittest.TestCase):
    def test_memory_bus_read_ram_bytes(self):
        source = """#include <iostream>
#include <cassert>
#include <vector>
#include <optional>
#include <cstdint>
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/dev/device.hpp"

class CountingDevice : public tinyarmsim::mmio::MMIODevice {
public:
    uint32_t base_{0};
    uint32_t span_{0};
    mutable int read_count = 0;
    CountingDevice(uint32_t base, uint32_t span) : base_(base), span_(span) {}
    uint32_t base_address() const noexcept override { return base_; }
    uint32_t address_span() const noexcept override { return span_; }
    uint32_t read(uint32_t, tinyarmsim::mmio::AccessSize) override {
        read_count++;
        return 0x42;
    }
    void write(uint32_t, uint32_t, tinyarmsim::mmio::AccessSize) override {}
    void tick(uint64_t) override {}
    void reset() override {}
};

int main() {
    constexpr size_t RAM_SIZE = 1024 * 1024; // 1MB
    tinyarmsim::MemoryBus bus(RAM_SIZE);

    // 1. Known bytes at RAM start
    bus.write8(0, 0xAA);
    bus.write8(1, 0xBB);
    bus.write8(2, 0xCC);
    bus.write8(3, 0xDD);

    auto snap_start = bus.read_ram_bytes(0, 4);
    assert(snap_start.has_value());
    assert(snap_start->size() == 4);
    assert((*snap_start)[0] == 0xAA);
    assert((*snap_start)[1] == 0xBB);
    assert((*snap_start)[2] == 0xCC);
    assert((*snap_start)[3] == 0xDD);

    // 2. Known bytes at RAM end
    uint32_t end_addr = static_cast<uint32_t>(RAM_SIZE - 4);
    bus.write8(end_addr, 0x11);
    bus.write8(end_addr + 1, 0x22);
    bus.write8(end_addr + 2, 0x33);
    bus.write8(end_addr + 3, 0x44);

    auto snap_end = bus.read_ram_bytes(end_addr, 4);
    assert(snap_end.has_value());
    assert(snap_end->size() == 4);
    assert((*snap_end)[0] == 0x11);
    assert((*snap_end)[1] == 0x22);
    assert((*snap_end)[2] == 0x33);
    assert((*snap_end)[3] == 0x44);

    // 3. Exact limit 64
    uint32_t max_64_start = static_cast<uint32_t>(RAM_SIZE - 64);
    for (size_t i = 0; i < 64; ++i) {
        bus.write8(max_64_start + i, static_cast<uint8_t>(i));
    }
    auto snap_64 = bus.read_ram_bytes(max_64_start, 64);
    assert(snap_64.has_value());
    assert(snap_64->size() == 64);
    for (size_t i = 0; i < 64; ++i) {
        assert((*snap_64)[i] == static_cast<uint8_t>(i));
    }

    // 4. Zero, 65, SIZE_MAX, and uint32 max rejected
    assert(!bus.read_ram_bytes(0, 0).has_value());
    assert(!bus.read_ram_bytes(0, 65).has_value());
    assert(!bus.read_ram_bytes(100, 65).has_value());
    assert(!bus.read_ram_bytes(0, SIZE_MAX).has_value());
    assert(!bus.read_ram_bytes(0xFFFFFFFFu, 1).has_value());
    assert(!bus.read_ram_bytes(0xFFFFFFFFu, 64).has_value());
    assert(!bus.read_ram_bytes(static_cast<uint32_t>(RAM_SIZE), 1).has_value());
    // Span exceeding RAM boundary
    assert(!bus.read_ram_bytes(static_cast<uint32_t>(RAM_SIZE - 10), 11).has_value());

    // 5. Registered test device inside RAM range must receive 0 read calls for rejected snapshot
    // Register device at 0x1000 with span 0x100
    auto dev = std::make_unique<CountingDevice>(0x1000, 0x100);
    CountingDevice* raw_dev = dev.get();
    bus.register_device(std::move(dev));

    // Full overlap inside MMIO slot
    assert(!bus.read_ram_bytes(0x1000, 16).has_value());
    assert(raw_dev->read_count == 0);

    assert(!bus.read_ram_bytes(0x1010, 32).has_value());
    assert(raw_dev->read_count == 0);

    // Partial overlap before slot: [0x0FF0, 0x0FF0 + 32) overlaps [0x1000, 0x1100)
    assert(!bus.read_ram_bytes(0x0FF0, 32).has_value());
    assert(raw_dev->read_count == 0);

    // Partial overlap after slot: [0x10F0, 0x10F0 + 32) overlaps [0x1000, 0x1100)
    assert(!bus.read_ram_bytes(0x10F0, 32).has_value());
    assert(raw_dev->read_count == 0);

    // Just adjacent before slot: [0x0FE0, 0x0FE0 + 32 = 0x1000) does not overlap [0x1000, 0x1100)
    auto snap_adj_before = bus.read_ram_bytes(0x0FE0, 32);
    assert(snap_adj_before.has_value());
    assert(raw_dev->read_count == 0);

    // Just adjacent after slot: [0x1100, 0x1100 + 32) does not overlap [0x1000, 0x1100)
    auto snap_adj_after = bus.read_ram_bytes(0x1100, 32);
    assert(snap_adj_after.has_value());
    assert(raw_dev->read_count == 0);

    std::cout << "OK" << std::endl;
    return 0;
}
"""
        proc = run_cpp(source)
        self.assertEqual(proc.returncode, 0)
        self.assertIn("OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()
