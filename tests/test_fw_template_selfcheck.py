import pathlib
import shutil
import subprocess
import tempfile
import unittest

from tests.fw_control_test_support import controlled_firmware


class FirmwareTemplateSelfCheckTest(unittest.TestCase):
    def _build_and_check(self, source: pathlib.Path, opt: str, expected: int) -> None:
        with tempfile.TemporaryDirectory(prefix="fw-template-selfcheck-") as temp_dir:
            template = pathlib.Path(temp_dir) / "template"
            shutil.copytree(source, template)
            if expected != 47:
                config = template / "fw_config.h"
                config.write_text(config.read_text().replace(
                    "#define FW_SELF_CHECK_EXPECTED 47u",
                    f"#define FW_SELF_CHECK_EXPECTED {expected}u",
                ))

            build = subprocess.run(
                ["make", f"OPT={opt}"], cwd=template, capture_output=True,
                text=True, timeout=15,
            )
            self.assertEqual(build.returncode, 0,
                             f"Build failed for {opt}:\n{build.stdout}\n{build.stderr}")
            elf = template / "template.elf"
            nm = shutil.which("arm-none-eabi-nm") or "/usr/bin/arm-none-eabi-nm"
            symbols_result = subprocess.run(
                [nm, str(elf)], capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(symbols_result.returncode, 0, symbols_result.stderr)
            symbols = {
                parts[2]: int(parts[0], 16)
                for line in symbols_result.stdout.splitlines()
                if len(parts := line.split()) == 3
            }
            for name in ("g_counter", "g_self_check_result", "g_self_check_status"):
                self.assertIn(name, symbols, f"Missing {name} in nm output:\n{symbols_result.stdout}")

            with controlled_firmware(
                initial_commands="pause\n", elf_path=elf,
                max_steps=1000, host_timeout=10,
            ) as session:
                self.assertEqual(session.receive(), {"event": "paused"})
                for _ in range(64):
                    session.send("step\n")
                    self.assertEqual(session.receive(), {"event": "stepped"})

                observed = {}
                for name in ("g_counter", "g_self_check_result", "g_self_check_status"):
                    session.send(f"mem {symbols[name]} 4\n")
                    reply = session.receive()
                    self.assertEqual(reply.get("event"), "memory", reply)
                    observed[name] = int.from_bytes(bytes(reply["bytes"]), "little")

                self.assertGreater(observed["g_counter"], 42)
                self.assertEqual(observed["g_self_check_result"], 47)
                self.assertEqual(observed["g_self_check_status"], 1 if expected == 47 else 0)
                session.send("stop\n")
                self.assertEqual(session.receive(), {"event": "stopped", "reason": "requested"})
                session.communicate(timeout=1)
                self.assertEqual(session.process.returncode, 0)

    def test_self_check_o0_and_o2_and_negative_expectation(self):
        source = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "template"
        self._build_and_check(source, "-O0", 47)
        self._build_and_check(source, "-O2", 47)
        self._build_and_check(source, "-O2", 48)


if __name__ == "__main__":
    unittest.main()
