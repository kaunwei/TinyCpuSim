import os
import shutil
import subprocess
import tempfile
import unittest

CPP_SRC = """
#include "tinyarmsim/utils/firmware_signal_guard.hpp"
#include <cassert>
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdexcept>

static volatile sig_atomic_t g_prev_int = 0;
static void prev_int_handler(int sig) {
    g_prev_int = sig;
}

int test_normal_and_mask_restore() {
    struct sigaction sa_orig{}, sa_old{};
    sa_orig.sa_handler = prev_int_handler;
    sigemptyset(&sa_orig.sa_mask);
    sa_orig.sa_flags = 0;
    assert(sigaction(SIGINT, &sa_orig, &sa_old) == 0);

    sigset_t orig_mask;
    assert(pthread_sigmask(SIG_SETMASK, nullptr, &orig_mask) == 0);
    assert(sigismember(&orig_mask, SIGINT) == 0);

    // Block SIGTERM initially to verify it remains blocked after guard destruction
    sigset_t test_mask = orig_mask;
    sigaddset(&test_mask, SIGTERM);
    assert(pthread_sigmask(SIG_SETMASK, &test_mask, nullptr) == 0);

    {
        tinyarmsim::FirmwareSignalGuard guard;
        assert(guard.received_signal() == 0);

        // Raise SIGINT -> should be captured by guard
        assert(raise(SIGINT) == 0);
        assert(guard.received_signal() == SIGINT);

        // Subsequent signal shouldn't overwrite first
        assert(raise(SIGTERM) == 0);
        assert(guard.received_signal() == SIGINT);
    }

    // Previous handler was NOT called during guard
    assert(g_prev_int == 0);

    // After guard destroyed, verify original handler restored
    struct sigaction sa_current{};
    assert(sigaction(SIGINT, nullptr, &sa_current) == 0);
    assert(sa_current.sa_handler == prev_int_handler);

    // Verify mask restored (SIGTERM is blocked, SIGINT is not blocked)
    sigset_t current_mask;
    assert(pthread_sigmask(SIG_SETMASK, nullptr, &current_mask) == 0);
    assert(sigismember(&current_mask, SIGTERM) == 1);
    assert(sigismember(&current_mask, SIGINT) == 0);

    // Clean up
    assert(sigaction(SIGINT, &sa_old, nullptr) == 0);
    assert(pthread_sigmask(SIG_SETMASK, &orig_mask, nullptr) == 0);
    return 0;
}

int test_sigterm_first() {
    tinyarmsim::FirmwareSignalGuard guard;
    assert(guard.received_signal() == 0);
    assert(raise(SIGTERM) == 0);
    assert(guard.received_signal() == SIGTERM);
    return 0;
}

int test_overlap_rejected() {
    tinyarmsim::FirmwareSignalGuard guard1;
    bool caught = false;
    try {
        tinyarmsim::FirmwareSignalGuard guard2;
    } catch (const std::exception&) {
        caught = true;
    }
    assert(caught);
    // guard1 should still be functional
    assert(raise(SIGINT) == 0);
    assert(guard1.received_signal() == SIGINT);
    return 0;
}

int test_exception_cleanup() {
    try {
        tinyarmsim::FirmwareSignalGuard guard1;
        throw std::runtime_error("test");
    } catch (...) {
    }
    // Should be able to create guard again
    tinyarmsim::FirmwareSignalGuard guard2;
    assert(guard2.received_signal() == 0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    int mode = argv[1][0] - '0';
    switch (mode) {
        case 1: return test_normal_and_mask_restore();
        case 2: return test_sigterm_first();
        case 3: return test_overlap_rejected();
        case 4: return test_exception_cleanup();
        default: return 2;
    }
}
"""

class TestFirmwareSignalGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.mkdtemp(prefix="test_fw_signal_guard_")
        cls.bin_path = os.path.join(cls.tmpdir, "test_guard")
        src_path = os.path.join(cls.tmpdir, "main.cpp")
        with open(src_path, "w") as f:
            f.write(CPP_SRC)

        include_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "include"))
        cmd = ["/usr/bin/c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{include_dir}", src_path, "-o", cls.bin_path]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Compilation failed: {res.stderr}")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmpdir, ignore_errors=True)

    def test_signals_are_recorded_and_previous_actions_and_mask_are_restored(self):
        res = subprocess.run([self.bin_path, "1"], timeout=10)
        self.assertEqual(res.returncode, 0)

    def test_sigterm_recorded(self):
        res = subprocess.run([self.bin_path, "2"], timeout=10)
        self.assertEqual(res.returncode, 0)

    def test_overlapping_guard_rejected(self):
        res = subprocess.run([self.bin_path, "3"], timeout=10)
        self.assertEqual(res.returncode, 0)

    def test_exception_unwind_restores_state(self):
        res = subprocess.run([self.bin_path, "4"], timeout=10)
        self.assertEqual(res.returncode, 0)

if __name__ == "__main__":
    unittest.main()
