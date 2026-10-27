import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest
from tests.fw_control_test_support import controlled_firmware


class TestFwControlSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(
            ["cmake", "--build", "build", "--target", "tinycpusim", "-j2"],
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def test_preloaded_stop_ack_and_exit_zero(self):
        with controlled_firmware(initial_commands="stop\n") as session:
            msg = session.receive()
            self.assertEqual(msg, {"event": "stopped", "reason": "requested"})
            stdout, stderr = session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)

    def test_caller_exception_cleans_up_process(self):
        proc_ref = None
        with self.assertRaises(RuntimeError):
            with controlled_firmware(initial_commands="stop\n") as session:
                proc_ref = session.process
                raise RuntimeError("caller error")
        self.assertIsNotNone(proc_ref)
        self.assertIsNotNone(proc_ref.poll())

    def test_alternate_elf_ram_marker_is_read_while_paused(self):
        default_elf = pathlib.Path(__file__).resolve().parent.parent / "build" / "isa_boot_acceptance.elf"
        # The firmware loader uses the data segment's physical load address.
        marker_address = 0x101AC
        marker_value = 0x2468ACE0

        with tempfile.TemporaryDirectory() as directory:
            alternate_elf = pathlib.Path(directory) / "alternate.elf"
            shutil.copyfile(default_elf, alternate_elf)
            elf_bytes = bytearray(alternate_elf.read_bytes())
            # In the acceptance ELF, initialized_value is the first .data word:
            # the RAM load segment maps file offset 0x2000 to 0x20000000.
            struct.pack_into("<I", elf_bytes, 0x2000, marker_value)
            alternate_elf.write_bytes(elf_bytes)

            with controlled_firmware(initial_commands="pause\n", elf_path=alternate_elf) as session:
                self.assertEqual(session.receive(), {"event": "paused"})
                session.send(f"mem 0x{marker_address:x} 4\n")
                self.assertEqual(session.receive(), {
                    "event": "memory",
                    "address": marker_address,
                    "bytes": list(struct.pack("<I", marker_value)),
                })

                session.send("stop\n")
                self.assertEqual(session.receive(), {"event": "stopped", "reason": "requested"})
                stdout, stderr = session.communicate(timeout=2.0)
                self.assertEqual(session.process.returncode, 0, f"stdout={stdout}\nstderr={stderr}")


if __name__ == "__main__":
    unittest.main()
