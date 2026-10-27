# Firmware Debug Environment Baseline

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

3. **Current vs. Planned Capabilities**:
   - **Existing**: `--max-steps` bounded execution reporting exit code 1 with explicit error message, instruction execution tracing via `--log`, and independent subprocess host timeouts in test harnesses (`tests/test_fw_step_limit.py`).
   - **Planned**: Target-service progress watchdog, interactive pause/step/state-inspection controls, live UART bidirectional transport, and graceful controlled-stop interface.

## Minimal Copied Firmware Debugger Walkthrough (O0 and O2)

The automated end-to-end walkthrough is `tests/test_fw_debug_walkthrough.py`. It copies the unchanged `firmware/template` to a temporary directory, builds it with `make OPT=-O0` and `make OPT=-O2`, and uses `nm` to obtain `main` and `g_counter`. `objdump -d` supplies instruction addresses and encoded widths. The normal simulator is built once by the test setup (`cmake --build build --target tinycpusim -j2`, bounded to 120 seconds). Each controlled run uses the copied ELF with a 1000-step ceiling and a 10-second host timeout.

Reproduce from the repository root:

```bash
/usr/bin/python3 -m unittest discover -s tests -p test_fw_debug_walkthrough.py -v
```

For each optimization level, the test stops at `main` before its first instruction, checks PC and the positive startup instruction count, reads the initial four bytes at `g_counter`, executes exactly one instruction, and checks the resulting PC against the independently decoded `objdump` width and the step count against the startup count plus one. It then places a breakpoint at the next disassembled instruction after that PC and continues; the reported hit must follow at least one additional executed instruction. Finally it requests `stop` and requires the requested-stop acknowledgement, simulator exit code zero, and context-managed process/socket cleanup. A limit exhaustion or timeout fails the walkthrough.

Observed on this toolchain for both copied builds: `nm` reports `main=000100cc` and `g_counter=00020000`. O0 begins `0x100cc: b480 push {r7}` followed by `0x100ce: af00 add r7, sp, #0`; O2 begins `0x100cc: 232a movs r3, #42` followed by `0x100ce: 4a03 ldr r2, [pc, #12]`. The controlled run reaches `main` after 16 startup instructions. The required unittest command completed with 2 tests passing (O0 and O2), exit code 0.

This demonstrates **instruction stepping**: one simulator instruction advances the architectural PC by the encoded instruction width (2 or 4 bytes here). It does not provide **source stepping**, which would group machine instructions according to compiler debug-line mappings and stop on source lines. Optimization can change that mapping and instruction sequence; this walkthrough makes no source-line stepping claim.
