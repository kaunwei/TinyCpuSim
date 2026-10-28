# Minimal firmware template

This directory is a small Cortex-M3 firmware starting point. It contains one
configuration header, a startup file, a linker script, and one C source file.
It is not a full firmware guide, scheduler, or UART-to-PTY integration.

## Copy and build

From the repository root, copy the whole directory, then build either ordinary
optimization profile:

```sh
cp -R firmware/template /tmp/my-firmware
cd /tmp/my-firmware
make clean
make OPT=-O0
make clean
make OPT=-O2
```

Each build creates `template.elf`. `make clean` removes that file. The Makefile
uses `arm-none-eabi-gcc`, Cortex-M3 Thumb mode, freestanding/no-stdlib flags,
`-g`, and `-Wall -Wextra`; `OPT` defaults to `-O0`. No ISA workaround flags
are needed for the current source.

`fw_config.h` is the template's single configuration header. The self-check
uses `FW_SELF_CHECK_INITIAL_COUNTER` (42), `FW_SELF_CHECK_INCREMENT` (5), and
`FW_SELF_CHECK_EXPECTED` (47 by default). It stores the computed value in
`g_self_check_result`, initializes `g_self_check_status` to
`FW_SELF_CHECK_FAIL` (0), then stores `FW_SELF_CHECK_PASS` (1) when the result
matches the expected value. To exercise the negative expectation, change the
default expected value to 48; result remains 47 and status remains FAIL (0).

## Inspect the ELF

Use the symbols and instructions from the build you just made; compiler versions
can change addresses and instruction selection. With the repository's GNU Arm
toolchain at revision `d511243`, both `-O0` and `-O2` builds reported:

```text
000100cc T main
00020000 B g_counter
00020004 B g_self_check_result
00020008 B g_self_check_status
```

Inspect them with:

```sh
arm-none-eabi-nm -n template.elf
arm-none-eabi-objdump -d template.elf
```

At those observed addresses, O0 begins `main` with `push {r7}` at `0x100cc`;
O2 begins with `movs r3, #42` at `0x100cc`. The ordinary conditional is
present in O2 as `cmp r3, #47` at `0x100de`, followed by `itt eq`,
`moveq r3, #1`, and `streq r3, [r2, #8]` at `0x100e0–0x100e4`.

## Execution and bounded control

`main` performs the self-check and then deliberately runs an infinite counter
loop. The firmware does not terminate on PASS or FAIL. Run it under a harness
that starts `tinycpusim --firmware --control-fd FD --max-steps 1000
template.elf`, passes an inherited control socket as `FD`, observes the guest
state, and sends the newline-terminated `stop` command when its checks are
complete. Require the `stopped` response with reason `requested` and a zero
simulator exit status. The existing walkthrough test demonstrates this
socket-based control pattern in `tests/test_fw_debug_walkthrough.py`.

A step limit is a bound, not a successful completion signal: if it expires
before an external stop, TinyCpuSim reports a step-limit failure. The
self-check's current O0/O2 and negative-expectation coverage is in
`tests/test_fw_template_selfcheck.py`.
