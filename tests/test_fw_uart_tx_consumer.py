import unittest

from tests.fw_cpp_test_support import run_cpp


class TestFwUartTxConsumer(unittest.TestCase):
    def test_completion_capture_and_optional_consumer_lifecycle(self):
        source = r'''#include "tinyarmsim/mmio/uart.hpp"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
using tinyarmsim::mmio::AccessSize;
using tinyarmsim::mmio::UartDevice;
int main() {
    UartDevice uart;
    uart.reset();
    uart.write(0x00, 'A', AccessSize::Word);
    assert(uart.get_tx_output() == "A");

    uart.write(0x10, 2, AccessSize::Word);
    std::vector<uint8_t> delivered;
    uart.set_tx_consumer([&](uint8_t byte) { delivered.push_back(byte); });
    uart.write(0x00, 0, AccessSize::Word);
    uart.tick(19);
    assert(delivered.empty());
    uart.tick(1);
    assert((delivered == std::vector<uint8_t>{0}));
    uart.tick(20);
    assert(delivered.size() == 1);
    assert(uart.get_tx_output() == "A");

    for (unsigned value = 0; value <= 255; ++value) {
        uart.write(0x00, value, AccessSize::Word);
        uart.tick(20);
    }
    assert(delivered.size() == 257);
    assert(delivered[1] == 0);
    assert(delivered.back() == 255);
    assert(uart.get_tx_output() == "A");

    uart.set_tx_consumer({});
    uart.write(0x00, 0x7f, AccessSize::Word);
    uart.tick(20);
    assert(delivered.size() == 257);
    assert(uart.get_tx_output() == std::string("A\x7f", 2));

    uart.write(0x08, 0, AccessSize::Word);
    uart.write(0x00, 0x33, AccessSize::Word);
    uart.tick(20);
    assert(uart.get_tx_output() == std::string("A\x7f", 2));
    uart.write(0x08, 1, AccessSize::Word);
    uart.write(0x10, 2, AccessSize::Word);
    uart.write(0x00, 0x11, AccessSize::Word);
    uart.write(0x00, 0x22, AccessSize::Word);
    uart.tick(20);
    assert(uart.get_tx_output() == std::string("A\x7f\x11", 3));

    uart.set_tx_consumer([&](uint8_t byte) { delivered.push_back(byte); });
    uart.reset();
    uart.write(0x10, 2, AccessSize::Word);
    uart.write(0x00, 0xff, AccessSize::Word);
    uart.tick(20);
    assert(delivered.back() == 255);
    assert(uart.get_tx_output().empty());
    return 0;
}'''
        proc = run_cpp(source)
        self.assertEqual(proc.returncode, 0, proc.stderr)
