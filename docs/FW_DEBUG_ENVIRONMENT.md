# Firmware Debug Environment Baseline

## Historical Host and Release Build Baseline

Evidence collected from the existing checkout and artifacts on 2026-10-28; no build or configuration was changed.

- **Host OS:** `uname -a` reported `Linux shadow 6.8.0-139-generic #139-Ubuntu SMP PREEMPT_DYNAMIC Sat Aug 1 03:52:05 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux`.
- **Normal Arm tools:** `/usr/bin/arm-none-eabi-gcc --version` reported `arm-none-eabi-gcc (15:13.2.rel1-2) 13.2.1 20231009`; `/usr/bin/arm-none-eabi-objdump --version` reported `GNU objdump (2.42-1ubuntu1+23) 2.42`.
- **Simulator revision/version at historical observation:** repository `HEAD` was `f89e219`; the existing `./build/tinycpusim` identified itself as `TinyCpuSim v0.1.0` in its usage output. `--version` is not a supported option.
- **Existing CMake build:** `build/CMakeCache.txt` reports `CMAKE_BUILD_TYPE:STRING=Release` (only existing cache found). Debug flags are recorded as `-g`, but this build is not Debug; a Debug build baseline is therefore unresolved.
- **Existing ELF debug evidence:** `/usr/bin/arm-none-eabi-objdump -h build/isa_boot_acceptance.elf` lists `.text`, `.data`, `.bss`, `.ARM.attributes`, and `.comment`, with no `.debug_*` sections. The symbol table contains linker/source names and functions such as `main` (`0x000100cc`) and `exercise_isa_families` (`0x00010114`), but no DWARF debug-section evidence. Source-level debug information is unresolved; these symbols do not establish source-line debugging.

Reproduce the inspected build and ELF evidence without reconfiguring:

```bash
uname -a
/usr/bin/arm-none-eabi-gcc --version
/usr/bin/arm-none-eabi-objdump --version
rg -n 'CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS_DEBUG|CMAKE_C_FLAGS_DEBUG' build/CMakeCache.txt
/usr/bin/arm-none-eabi-objdump -h build/isa_boot_acceptance.elf
/usr/bin/arm-none-eabi-objdump -t build/isa_boot_acceptance.elf
```

## Toolchain

Both requested GNU Arm Embedded tools are available in `/usr/bin`:

- `arm-none-eabi-gcc`: 13.2.1 (GCC 15:13.2.1-2, dated 20231009)
- `arm-none-eabi-objdump`: 2.42 (GNU objdump 2.42-1ubuntu1+23)

## Symbol and Disassembly Inspection Workflow

Using the existing minimal firmware fixture `build/isa_boot_acceptance.elf`:

### Symbol Inspection (`arm-none-eabi-objdump -t`)

Command:
```bash
/usr/bin/arm-none-eabi-objdump -t build/isa_boot_acceptance.elf
```

Observed output (excerpt):
```text
build/isa_boot_acceptance.elf:     file format elf32-littlearm

SYMBOL TABLE:
00010000 l    d  .text	00000000 .text
00020000 l    d  .data	00000000 .data
00020004 l    d  .bss	00000000 .bss
...
00010080 g     F .text	00000000 Reset_Handler
000100b0 g       .text	00000000 Default_Handler
000100cc g     F .text	00000048 main
00010114 g     F .text	00000098 exercise_isa_families
00020004 g     O .bss	00000034 acceptance_results
00030000 g       .text	00000000 _stack_top
```

### Disassembly Inspection (`arm-none-eabi-objdump -d`)

Command:
```bash
/usr/bin/arm-none-eabi-objdump -d build/isa_boot_acceptance.elf
```

Observed disassembly starting at `Reset_Handler` (`0x00010080`):
```text
00010080 <Reset_Handler>:
   10080:	f8df d030 	ldr.w	sp, [pc, #48]	@ 100b4 <Default_Handler+0x4>
   10084:	480c      	ldr	r0, [pc, #48]	@ (100b8 <Default_Handler+0x8>)
   10086:	490d      	ldr	r1, [pc, #52]	@ (100bc <Default_Handler+0xc>)
   10088:	4a0d      	ldr	r2, [pc, #52]	@ (100c0 <Default_Handler+0x10>)
   1008a:	4288      	cmp	r0, r1
   1008c:	d204      	bcs.n	10098 <Reset_Handler+0x18>
   1008e:	f852 3b04 	ldr.w	r3, [r2], #4
   10092:	f840 3b04 	str.w	r3, [r0], #4
   10096:	e7f8      	b.n	1008a <Reset_Handler+0xa>
   10098:	480a      	ldr	r0, [pc, #40]	@ (100c4 <Default_Handler+0x14>)
   1009a:	490b      	ldr	r1, [pc, #44]	@ (100c8 <Default_Handler+0x18>)
   1009c:	2200      	movs	r2, #0
   1009e:	4288      	cmp	r0, r1
   100a0:	d202      	bcs.n	100a8 <Reset_Handler+0x28>
   100a2:	f840 2b04 	str.w	r2, [r0], #4
   100a6:	e7fa      	b.n	1009e <Reset_Handler+0x1e>
   100a8:	f000 f810 	bl	100cc <main>
   100ac:	df00      	svc	0
   100ae:	e7fe      	b.n	100ae <Reset_Handler+0x2e>
```

## Bounded Execution Trace and Expected Step-Limit Failure

Executing `tinycpusim` with `--firmware`, instruction trace logging (`--log`), and a small explicit instruction limit (`--max-steps 5`):

Command:
```bash
./build/tinycpusim --firmware --log --max-steps 5 build/isa_boot_acceptance.elf
```

Exit code: `1`

Observed stdout/stderr:
```text
TinyCpuSim v0.1.0
Loading ELF: build/isa_boot_acceptance.elf...
Entry Point: 0x10080
Starting simulation...
core 0: 0x00010080 (0xf8dfd030)  ldr sp, [pc, #48]        | sp 0x00030000 | mem[0x100b4] => 0x30000 (4B) | NZCV=[0000]
core 0: 0x00010084 (0x480c)  ldr r0, [pc, #48]        | r0 0x00020000 | mem[0x100b8] => 0x20000 (4B) | NZCV=[0000]
core 0: 0x00010086 (0x490d)  ldr r1, [pc, #52]        | r1 0x00020004 | mem[0x100bc] => 0x20004 (4B) | NZCV=[0000]
core 0: 0x00010088 (0x4a0d)  ldr r2, [pc, #52]        | r2 0x000101ac | mem[0x100c0] => 0x101ac (4B) | NZCV=[0000]
core 0: 0x0001008a (0x4288)  cmp r0, r1               | NZCV=[1000]

============================================================
SIMULATION FAILED
Fault Reason:       Simulation execution exceeded max step limit (5)
Exit Code:          1 (R0 = 1)
Total Instructions: 5
Elapsed Time:       0.000050 s
Simulation Speed:   100371.37 inst/s (0.10 MIPS)
============================================================
```

## Execution Limits and Stop/Timeout Policy

As established in `todo/fw-development/README.md`:

1. **Infinite-Loop Execution Contract**:
   - Production firmware enters an infinite main loop after initialization. CPU execution does not stop at a particular C source line.
   - Interactive execution, debugger pause, and automated test completion are distinct controls. Pausing freezes simulated time; exiting cleans processes and terminal resources.

2. **Successful Stop vs. Step-Limit Failure vs. Watchdog/Host Timeout**:
   - **Successful Stop**: Automated test cases declare an observable success predicate (e.g., correct command output, target-service completion, or verified heartbeats). The test harness requests a controlled simulator stop only after all required assertions pass and verifies cleanup.
   - **Step-Limit Exhaustion**: Every case specifies a small explicit total instruction-step budget justified by its workload (never relying on the default CLI limit of one billion steps). Exhausting this step budget is a failure (`SIMULATION FAILED: Simulation execution exceeded max step limit`), not a success. Dedicated negative tests may pass only by asserting the documented failure behavior.
   - **Target-Service Progress Watchdog**: A bounded simulated-step progress deadline detects missing progress early. A heartbeat alone cannot establish the health of unrelated services.
   - **Independent Host Wall-Clock Timeout**: Catches simulator/IO hangs, terminates/reaps the entire test process group, restores terminal state, and retains necessary diagnostics. Intentional debug pauses freeze simulated time and must never silently extend automated deadlines.

3. **Current vs. Planned Capabilities** (source state checked at revision `e029830`, 2026-10-28):

   | Capability | State at `e029830` | Evidence / limit |
   |---|---|---|
   | Quiet output | Existing | `--quiet` redirects `std::cout` in `src/main.cpp`; firmware UART's buffered TX string is printed after simulation (`src/main.cpp`). |
   | Controlled stop | Existing with firmware control FD | `stop` on `--control-fd` requests a normal stop and emits a `stopped` acknowledgement (`include/tinyarmsim/firmware_control.hpp`). Without this control channel, `--max-steps` exhaustion remains a failure. |
   | Pause and register inspection | Existing with firmware control FD | `pause`, `continue`, and `regs` commands; the register response includes PC, registers, and instruction count (`include/tinyarmsim/firmware_control.hpp`, `src/main.cpp`). |
   | Single instruction step | Existing with firmware control FD | `step` is accepted while paused; the interpreter runs one instruction before reporting `stepped` (`include/tinyarmsim/firmware_control.hpp`, `tests/test_fw_debug_walkthrough.py`). |
   | Memory inspection | Existing with firmware control FD | `mem <address> <length>` returns 1–64 bytes through the RAM provider; out-of-range or MMIO requests are rejected (`include/tinyarmsim/firmware_control.hpp`, `src/main.cpp`). |
   | Breakpoints | Existing, one address | `break <address>` and `break clear` pause when the PC reaches the selected address (`include/tinyarmsim/firmware_control.hpp`; exercised by `tests/test_fw_debug_walkthrough.py`). |
   | Live UART/PTY adapter | Missing | UART receives only through device-level `inject_rx`, accumulates completed TX characters in `tx_output_`, then `main` prints them after execution. No live UART-to-PTY adapter is connected (`include/tinyarmsim/mmio/uart.hpp`, `src/main.cpp`). |
   | Target-service progress watchdog | Missing | No service-specific simulated-step watchdog is established by the inspected implementation. |
   | Service watchdog | Missing | No independent service-health watchdog is established by the inspected implementation. |

   The UART is the firmware data channel: its MMIO DATA register carries guest bytes. The `--control-fd` socket is a separate inherited host control channel for newline-delimited commands and JSON events; it does not carry UART bytes. A PTY-capable host therefore does not imply a simulator PTY adapter.

### PTY/Terminal Host Capability Probe

On 2026-10-28, a bounded Python `os.openpty()` / `termios.tcgetattr()` probe succeeded on this host:

```text
master_fd=3 path='/dev/ptmx'
slave_fd=4 path='/dev/pts/5'
termios.tcgetattr(slave): ok, 7 attribute fields
closed slave_fd=4
closed master_fd=3
```

The probe closed both descriptors it opened. This verifies that this process could allocate a PTY pair and read slave terminal attributes at that time; it does not establish simulator PTY integration or establish that the PTY mount, path, or permissions will be identical in another execution environment. Any future terminal adapter must restore saved terminal attributes and close its own PTY and control descriptors on normal stop, errors, and timeout.

An intentional debugger pause blocks command handling until resumed/stopped and does not advance interpreter instructions or simulated peripheral time while blocked. Automated tests must keep an independent host wall-clock deadline active during such pauses; pausing simulation must not suspend or extend the test harness's timeout. Process-group termination/reaping and terminal-state restoration remain harness responsibilities on timeout.

The host, toolchain, Release-build, ELF, and step-limit observations above are historical baseline evidence. They remain separate from the refreshed Debug observations below.

## Isolated Debug Build and Guest DWARF Evidence

On 2026-10-28, the existing isolated development build was rebuilt with the repository's default tools. Its original source revision is unknown; the verified rebuild below used current checkout revision `97a04c4` (`git rev-parse --short HEAD`). The task context records an earlier successful Debug rebuild at `4bd5180`, after `720af7a`; that is historical handoff evidence and does not identify the source revision of this refreshed build.

```bash
timeout --signal=TERM --kill-after=2s 120s cmake --build build/debug-baseline --target tinycpusim -j2
```

The bounded rebuild succeeded (`baremetal_firmware_elf` and `tinycpusim` targets built). The existing `build/debug-baseline/CMakeCache.txt` reports `CMAKE_BUILD_TYPE:STRING=Debug`, `CMAKE_CXX_FLAGS_DEBUG:STRING=-g`, and `CMAKE_C_FLAGS_DEBUG:STRING=-g`. `build/debug-baseline/tinycpusim --help` identifies the binary as `TinyCpuSim v0.1.0`; `--version` is unsupported (it prints usage and `Unknown option: --version`). This documents a verified rebuild at `97a04c4`; the source revision that produced the pre-existing build is unknown. No CMake configuration or build profile was changed. The existing root `build/` Release cache and artifacts were retained.

The existing minimal `firmware/template` sources and Makefile were copied unchanged to `build/debug-baseline/guest/` and built there with its existing `OPT=-O0` profile:

```bash
make -C build/debug-baseline/guest OPT=-O0
```

The emitted command used `-mcpu=cortex-m3 -mthumb -ffreestanding -nostdlib -O0 -g -Wall -Wextra -T linker.ld`; no ISA workaround or extra debug option was added. `/usr/bin/arm-none-eabi-readelf -S build/debug-baseline/guest/template.elf` reports `.debug_line` (size `0xd8`) and `.debug_info` (size `0xca`). `/usr/bin/arm-none-eabi-nm -n` reports `main=000100cc` and `g_counter=00020000`. `/usr/bin/arm-none-eabi-objdump -d` shows `main` starting `0x100cc: b480 push {r7}`, `0x100ce: af00 add r7, sp, #0`, and `0x100d0: 4b04 ldr r3, [pc, #16]`. `/usr/bin/arm-none-eabi-objdump --dwarf=decodedline` maps `startup.s` lines to instruction addresses, confirming the copied guest ELF contains DWARF line information. These section, symbol, and decoded-line checks were reconfirmed after the bounded rebuild.

For a bounded run, the Debug simulator executed the Debug guest with a five-instruction limit and a separate ten-second host timeout:

```bash
timeout --signal=TERM --kill-after=2s 10s build/debug-baseline/tinycpusim --firmware --log --max-steps 5 build/debug-baseline/guest/template.elf
```

It executed five instructions and reported `SIMULATION FAILED` / `Simulation execution exceeded max step limit (5)`, exiting 1. The host timeout did not fire. This is the expected step-limit failure, not a successful stop. This proves availability of guest debug information and instruction disassembly; the simulator walkthrough remains instruction stepping only and makes no source-stepping claim.

## Minimal Copied Firmware Debugger Walkthrough (O0 and O2)

The automated end-to-end walkthrough is `tests/test_fw_debug_walkthrough.py`. It copies the unchanged `firmware/template` to a temporary directory, builds it with `make OPT=-O0` and `make OPT=-O2`, and uses `nm` to obtain `main` and `g_counter`. `objdump -d` supplies instruction addresses and encoded widths. The normal simulator is built once by the test setup (`cmake --build build --target tinycpusim -j2`, bounded to 120 seconds). Each controlled run uses the copied ELF with a 1000-step ceiling and a 10-second host timeout.

Reproduce from the repository root:

```bash
/usr/bin/python3 -m unittest discover -s tests -p test_fw_debug_walkthrough.py -v
```

For each optimization level, the test stops at `main` before its first instruction, checks PC and the positive startup instruction count, reads the initial four bytes at `g_counter`, executes exactly one instruction, and checks the resulting PC against the independently decoded `objdump` width and the step count against the startup count plus one. It then places a breakpoint at the next disassembled instruction after that PC and continues; the reported hit must follow at least one additional executed instruction. Finally it requests `stop` and requires the requested-stop acknowledgement, simulator exit code zero, and context-managed process/socket cleanup. A limit exhaustion or timeout fails the walkthrough.

Observed on this toolchain for both copied builds: `nm` reports `main=000100cc` and `g_counter=00020000`. O0 begins `0x100cc: b480 push {r7}` followed by `0x100ce: af00 add r7, sp, #0`; O2 begins `0x100cc: 232a movs r3, #42` followed by `0x100ce: 4a03 ldr r2, [pc, #12]`. The controlled run reaches `main` after 16 startup instructions. The required unittest command completed with 2 tests passing (O0 and O2), exit code 0.

This demonstrates **instruction stepping**: one simulator instruction advances the architectural PC by the encoded instruction width (2 or 4 bytes here). It does not provide **source stepping**, which would group machine instructions according to compiler debug-line mappings and stop on source lines. Optimization can change that mapping and instruction sequence; this walkthrough makes no source-line stepping claim.

## Bounded Quiet-Fault and Host-Timeout Observations

These existing unittest suites were run on 2026-10-28 against the current checkout; they exercise bounded firmware fault reporting and host timeout/process cleanup without changing product code or build profiles.

Quiet undefined-instruction (UDF) behavior:

```bash
/usr/bin/python3 -m unittest discover -s tests -p test_fw_quiet_udf.py -v
```

Result: exit code 0; 1 test passed (`test_udf_reports_cpu_fault_on_stderr_without_stdout`). The test's simulator subprocess has a five-second timeout. It verifies quiet mode keeps stdout empty while reporting the CPU fault on stderr.

Host timeout and process-group cleanup:

```bash
/usr/bin/python3 -m unittest discover -s tests -p test_fw_host_timeout.py -v
```

Result: exit code 0; 7 tests passed. Coverage includes timeout killing/reaping the process group, descendants that inherit pipes, cleanup after direct child exit and caller interruption, exit-code preservation, bounded successful execution, and a real simulator step-limit run through `run_bounded`.

Required step-limit regression suite:

```bash
/usr/bin/python3 -m unittest discover -s tests -p test_fw_step_limit.py -v
```

Result: exit code 0; 1 test passed. It confirms firmware execution exceeding the explicit step limit exits with the expected error.
