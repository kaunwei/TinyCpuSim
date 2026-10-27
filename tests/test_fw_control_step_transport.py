import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlStepTransport(unittest.TestCase):

    def test_step_transport_failure_precedence(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <string>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    int sndbuf = 1024;
    assert(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);

    char chunk[256] = {0};
    while (true) {
        ssize_t ret = ::send(sv[0], chunk, sizeof(chunk), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (ret < 0) {
            assert(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
    }

    std::string preload = "pause\nstep\n";
    assert(::write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));

    tinyarmsim::FirmwareControl control(sv[0]);
    bool stopped = control.stop_requested();
    assert(stopped && control.transport_failed());

    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=3)
        self.assertEqual(
            proc.returncode,
            0,
            f"Failed with returncode {proc.returncode}.\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
