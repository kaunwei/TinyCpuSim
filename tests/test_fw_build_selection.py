import json
import os
import pathlib
import sys
import tempfile
import unittest

from tests.fw_control_test_support import _REPO_ROOT, controlled_firmware


def _create_mock_simulator(path: pathlib.Path, marker: str) -> None:
    code = f"""#!{sys.executable}
import json
import os
import pathlib
import socket
import sys

MARKER = {repr(marker)}

def main():
    control_fd = None
    args = sys.argv[1:]
    for i in range(len(args) - 1):
        if args[i] == "--control-fd":
            control_fd = int(args[i + 1])
            break

    elf = args[-1] if args else ""
    elf_content = ""
    try:
        elf_content = pathlib.Path(elf).read_text(encoding="utf-8", errors="ignore").strip()
    except Exception:
        pass

    if control_fd is not None:
        sock = socket.fromfd(control_fd, socket.AF_UNIX, socket.SOCK_STREAM)
        reader = sock.makefile("r", encoding="utf-8")
        while True:
            line = reader.readline()
            if not line:
                break
            cmd = line.strip()
            payload = {{
                "event": "stopped" if cmd == "stop" else "ack",
                "reason": "requested",
                "marker": MARKER,
                "sim_path": sys.argv[0],
                "elf_path": elf,
                "elf_content": elf_content,
                "child_env": os.environ.get("CHILD_TEST_VAR"),
            }}
            sock.sendall((json.dumps(payload) + "\\n").encode("utf-8"))
            if cmd == "stop":
                break
        sock.close()

if __name__ == "__main__":
    main()
"""
    path.write_text(code, encoding="utf-8")
    path.chmod(0o755)


class TestFwBuildSelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory(prefix="fw-build-sel-")
        cls.base_path = pathlib.Path(cls.temp_dir.name).resolve()

        cls.root_a = cls.base_path / "root_a"
        cls.root_b = cls.base_path / "root_b"
        cls.root_a.mkdir()
        cls.root_b.mkdir()

        _create_mock_simulator(cls.root_a / "tinycpusim", "PROFILE_A")
        (cls.root_a / "isa_boot_acceptance.elf").write_text("ELF_CONTENT_A\n", encoding="utf-8")

        _create_mock_simulator(cls.root_b / "tinycpusim", "PROFILE_B")
        (cls.root_b / "isa_boot_acceptance.elf").write_text("ELF_CONTENT_B\n", encoding="utf-8")

        cls.custom_elf = cls.base_path / "custom.elf"
        cls.custom_elf.write_text("CUSTOM_ELF_CONTENT\n", encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def setUp(self):
        self._orig_fw_root = os.environ.get("FW_TEST_BUILD_ROOT")
        if "FW_TEST_BUILD_ROOT" in os.environ:
            del os.environ["FW_TEST_BUILD_ROOT"]

    def tearDown(self):
        if self._orig_fw_root is not None:
            os.environ["FW_TEST_BUILD_ROOT"] = self._orig_fw_root
        elif "FW_TEST_BUILD_ROOT" in os.environ:
            del os.environ["FW_TEST_BUILD_ROOT"]

    def test_explicit_root_selection(self):
        with controlled_firmware(build_root=self.root_a, initial_commands="stop\n") as session:
            msg = session.receive()
            self.assertEqual(msg.get("event"), "stopped")
            self.assertEqual(msg.get("marker"), "PROFILE_A")
            self.assertEqual(msg.get("sim_path"), str(self.root_a / "tinycpusim"))
            self.assertEqual(msg.get("elf_path"), str(self.root_a / "isa_boot_acceptance.elf"))
            self.assertEqual(msg.get("elf_content"), "ELF_CONTENT_A")
            session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)

    def test_environment_root_selection(self):
        os.environ["FW_TEST_BUILD_ROOT"] = str(self.root_b)
        with controlled_firmware(initial_commands="stop\n") as session:
            msg = session.receive()
            self.assertEqual(msg.get("event"), "stopped")
            self.assertEqual(msg.get("marker"), "PROFILE_B")
            self.assertEqual(msg.get("sim_path"), str(self.root_b / "tinycpusim"))
            self.assertEqual(msg.get("elf_path"), str(self.root_b / "isa_boot_acceptance.elf"))
            self.assertEqual(msg.get("elf_content"), "ELF_CONTENT_B")
            session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)

    def test_explicit_elf_precedence(self):
        with controlled_firmware(
            build_root=self.root_a,
            elf_path=self.custom_elf,
            initial_commands="stop\n",
        ) as session:
            msg = session.receive()
            self.assertEqual(msg.get("event"), "stopped")
            self.assertEqual(msg.get("marker"), "PROFILE_A")
            self.assertEqual(msg.get("sim_path"), str(self.root_a / "tinycpusim"))
            self.assertEqual(msg.get("elf_path"), str(self.custom_elf))
            self.assertEqual(msg.get("elf_content"), "CUSTOM_ELF_CONTENT")
            session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)

    def test_unchanged_default_behavior(self):
        self.assertNotIn("FW_TEST_BUILD_ROOT", os.environ)
        with controlled_firmware(initial_commands="stop\n") as session:
            msg = session.receive()
            self.assertEqual(msg, {"event": "stopped", "reason": "requested"})
            session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)
            self.assertEqual(session.process.args[0], str(_REPO_ROOT / "build" / "tinycpusim"))
            self.assertEqual(session.process.args[-1], str(_REPO_ROOT / "build" / "isa_boot_acceptance.elf"))

    def test_env_passed_only_to_child(self):
        var_name = "CHILD_TEST_VAR"
        self.assertNotIn(var_name, os.environ)
        child_env = dict(os.environ)
        child_env[var_name] = "isolated_token_xyz"

        with controlled_firmware(
            build_root=self.root_a,
            env=child_env,
            initial_commands="stop\n",
        ) as session:
            msg = session.receive()
            self.assertEqual(msg.get("child_env"), "isolated_token_xyz")
            session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)

        self.assertNotIn(var_name, os.environ)

    def test_two_simultaneous_explicit_profiles_without_crosstalk(self):
        with controlled_firmware(
            build_root=self.root_a, initial_commands="", host_timeout=2.0
        ) as session_a, controlled_firmware(
            build_root=self.root_b, initial_commands="", host_timeout=2.0
        ) as session_b:
            alive = [session_a.process.poll() is None, session_b.process.poll() is None]
            self.assertEqual(alive, [True, True])
            self.assertIsNone(session_a.process.poll())
            self.assertIsNone(session_b.process.poll())
            self.assertNotEqual(session_a.process.pid, session_b.process.pid)
            self.assertEqual(session_a.process.args[0], str(self.root_a / "tinycpusim"))
            self.assertEqual(session_a.process.args[-1], str(self.root_a / "isa_boot_acceptance.elf"))
            self.assertEqual(session_b.process.args[0], str(self.root_b / "tinycpusim"))
            self.assertEqual(session_b.process.args[-1], str(self.root_b / "isa_boot_acceptance.elf"))
            self.assertNotEqual(session_a.process.args[0], session_b.process.args[0])
            self.assertNotEqual(session_a.process.args[-1], session_b.process.args[-1])

            session_a.send("stop\n")
            session_b.send("stop\n")
            msg_a = session_a.receive()
            msg_b = session_b.receive()

            self.assertEqual(msg_a.get("event"), "stopped")
            self.assertEqual(msg_a.get("reason"), "requested")
            self.assertEqual(msg_a.get("marker"), "PROFILE_A")
            self.assertEqual(msg_a.get("sim_path"), str(self.root_a / "tinycpusim"))
            self.assertEqual(msg_a.get("elf_path"), str(self.root_a / "isa_boot_acceptance.elf"))
            self.assertEqual(msg_a.get("elf_content"), "ELF_CONTENT_A")

            self.assertEqual(msg_b.get("event"), "stopped")
            self.assertEqual(msg_b.get("reason"), "requested")
            self.assertEqual(msg_b.get("marker"), "PROFILE_B")
            self.assertEqual(msg_b.get("sim_path"), str(self.root_b / "tinycpusim"))
            self.assertEqual(msg_b.get("elf_path"), str(self.root_b / "isa_boot_acceptance.elf"))
            self.assertEqual(msg_b.get("elf_content"), "ELF_CONTENT_B")

            stdout_a, stderr_a = session_a.communicate(timeout=2.0)
            stdout_b, stderr_b = session_b.communicate(timeout=2.0)
            self.assertEqual(session_a.process.returncode, 0)
            self.assertEqual(session_b.process.returncode, 0)
            self.assertEqual(stdout_a, "")
            self.assertEqual(stderr_a, "")
            self.assertEqual(stdout_b, "")
            self.assertEqual(stderr_b, "")

    def test_selected_missing_binary_must_fail(self):
        missing_root = self.base_path / "nonexistent_build_dir"
        missing_root.mkdir()

        # Explicit root missing binary
        with self.assertRaises(FileNotFoundError) as ctx_explicit:
            with controlled_firmware(build_root=missing_root, initial_commands="stop\n"):
                pass
        self.assertIn(str(missing_root / "tinycpusim"), str(ctx_explicit.exception))

        # Environment root missing binary
        os.environ["FW_TEST_BUILD_ROOT"] = str(missing_root)
        with self.assertRaises(FileNotFoundError) as ctx_env:
            with controlled_firmware(initial_commands="stop\n"):
                pass
        self.assertIn(str(missing_root / "tinycpusim"), str(ctx_env.exception))


if __name__ == "__main__":
    unittest.main()
