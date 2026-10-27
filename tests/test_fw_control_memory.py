import json
import unittest

from tests.fw_cpp_test_support import run_cpp


class TestFwControlMemory(unittest.TestCase):
    def test_memory_provider_returns_numeric_bytes_and_moves_with_owner(self):
        source = r'''#include "tinyarmsim/firmware_control.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
int main() {
    int sv[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl original(sv[0]);
    original.set_memory_provider([](uint32_t address, size_t length)
        -> std::optional<std::vector<uint8_t>> {
        assert(address == 0x20 && length == 4);
        return std::vector<uint8_t>{0, 1, 127, 255};
    });
    tinyarmsim::FirmwareControl moved(std::move(original));
    const std::string commands = "pause\nmem 0x20 4\nstop\n";
    assert(write(sv[1], commands.data(), commands.size()) == static_cast<ssize_t>(commands.size()));
    while (!moved.stop_requested()) {}
    char response[256] = {};
    ssize_t count = read(sv[1], response, sizeof(response));
    assert(count > 0);
    std::cout.write(response, count);
    close(sv[1]);
    return 0;
}'''
        proc = run_cpp(source)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('"event":"memory","address":32,"bytes":[0,1,127,255]', proc.stdout)

    def test_missing_provider_and_invalid_ranges_preserve_command_order(self):
        source = r'''#include "tinyarmsim/firmware_control.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
int main() {
    int sv[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    const std::string commands = "pause\nmem 0 1\nmem -1 1\nmem 0x100000000 1\nmem 0x20 0\nmem 0x20 65\nmem 0x20 1junk\nstop\n";
    assert(write(sv[1], commands.data(), commands.size()) == static_cast<ssize_t>(commands.size()));
    while (!control.stop_requested()) {}
    char response[1024] = {};
    ssize_t count = read(sv[1], response, sizeof(response));
    assert(count > 0);
    std::cout.write(response, count);
    close(sv[1]);
    return 0;
}'''
        proc = run_cpp(source)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        lines = [json.loads(line) for line in proc.stdout.splitlines() if line]
        self.assertEqual(lines[0], {"event": "paused"})
        self.assertEqual(lines[1], {"error": "missing memory provider"})
        self.assertEqual(lines[2:], [{"error": "invalid memory arguments"}] * 5 + [
            {"event": "stopped", "reason": "requested"}
        ])
