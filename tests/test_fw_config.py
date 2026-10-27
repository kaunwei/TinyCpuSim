import subprocess
import tempfile
import unittest
from pathlib import Path


class FirmwareConfigTest(unittest.TestCase):
    def test_fw_config_header_defaults_static_assert(self):
        project_root = Path(__file__).resolve().parents[1]
        include_dir = project_root / "firmware" / "template"
        header_path = include_dir / "fw_config.h"

        c_source = """
#include "fw_config.h"

_Static_assert(FW_TICKS_PER_SECOND == 1000000u, "FW_TICKS_PER_SECOND must be 1000000u");
_Static_assert(FW_TIMER_TASK_CAPACITY == 4u, "FW_TIMER_TASK_CAPACITY must be 4u");
_Static_assert(FW_MAIN_SERVICE_CAPACITY == 4u, "FW_MAIN_SERVICE_CAPACITY must be 4u");
_Static_assert(FW_UART_RX_CAPACITY == 128u, "FW_UART_RX_CAPACITY must be 128u");
_Static_assert(FW_UART_TX_CAPACITY == 128u, "FW_UART_TX_CAPACITY must be 128u");

_Static_assert(FW_TIMER_TASK_CAPACITY > 0u, "FW_TIMER_TASK_CAPACITY must be positive");
_Static_assert(FW_MAIN_SERVICE_CAPACITY > 0u, "FW_MAIN_SERVICE_CAPACITY must be positive");
_Static_assert(FW_UART_RX_CAPACITY > 0u, "FW_UART_RX_CAPACITY must be positive");
_Static_assert(FW_UART_TX_CAPACITY > 0u, "FW_UART_TX_CAPACITY must be positive");

int main(void) {
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            src_file = Path(temp_dir) / "test_config.c"
            src_file.write_text(c_source)

            # Compile with gcc (or arm-none-eabi-gcc if standard host gcc not preferred; host gcc is standard)
            # -I include_dir
            res = subprocess.run(
                ["gcc", "-Wall", "-Wextra", "-Werror", f"-I{include_dir}", "-c", str(src_file), "-o", str(Path(temp_dir) / "test_config.o")],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(res.returncode, 0, f"Compilation failed:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")
