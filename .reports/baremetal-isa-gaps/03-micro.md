# Cortex-M3 / Thumb-2 Bare-Metal ISA Readiness: Deep Technical Analysis

## 1. Scope & Objective
This audit examines TinyCpuSim's readiness to execute bare-metal C binaries compiled with ARM GCC (`-mcpu=cortex-m3 -mthumb -O2`). The codebase was audited across:
- `include/tinyarmsim/instruction.hpp`
- `include/tinyarmsim/decoder.hpp`
- `include/tinyarmsim/interpreter.hpp`
- `include/tinyarmsim/loader.hpp`
- `include/tinyarmsim/state.hpp`
- `firmware/` (`firmware/bsp/startup.s`, `firmware/bsp/linker.ld`, `firmware/Makefile`, `firmware/apps/demo_all_peripherals.c`)

---

## 2. In-Depth Instruction Audit & Encodings

### 2.1 Stack Frame Allocation & Base Relative Addressing (P0)
When compiling C code under Cortex-M3, function prologues and epilogues allocate stack space for local variables and spill registers.

1. **SP-relative ADD / SUB (`ADD/SUB sp, #imm7`)**:
   - **Encoding**:
     - `ADD sp, sp, #imm7`: `1011 0000 0 imm7` (`0xB000` | imm7, where imm = imm7 * 4).
     - `SUB sp, sp, #imm7`: `1011 0000 1 imm7` (`0xB080` | imm7, where imm = imm7 * 4).
   - **Codebase Status**:
     - In [instruction.hpp#L9-L60](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/instruction.hpp#L9-L60), `Opcode::ADD` and `Opcode::SUB` exist.
     - In [decoder.hpp#L286-L301](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp#L286-L301), pattern `0xB000` is completely skipped! The decoder checks `0xB400` (PUSH/POP) and `0xB100` (CBZ/CBNZ), but throws `UndefinedInstructionException` on `0xB0xx`.
     - In [interpreter.hpp#L193-L250](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/interpreter.hpp#L193-L250), `ADD` and `SUB` only target general registers or `write_reg(instr.rd, ...)`. If decoded properly with `rd = REG_SP, rn = REG_SP`, interpreter handles SP write via `write_reg`.
   - **Impact**: Any non-trivial C function with local variables crashes immediately at prologue (`sub sp, sp, #imm`).

2. **Form PC / SP Relative Address (`ADR Rd, #imm8` / `ADD Rd, SP, #imm8`)**:
   - **Encoding**:
     - `ADR Rd, #imm8`: `1010 0 Rd imm8` (`0xA000` | (Rd << 8) | imm8, where imm = imm8 * 4; Rd = PC + 4 + imm).
     - `ADD Rd, SP, #imm8`: `1010 1 Rd imm8` (`0xA800` | (Rd << 8) | imm8, where imm = imm8 * 4; Rd = SP + imm).
   - **Codebase Status**:
     - Missing in [decoder.hpp](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp) (`0xA000`-`0xAFFF` falls through to UndefinedInstructionException).
     - `Opcode::ADR` is missing from [instruction.hpp#L9-L60](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/instruction.hpp#L9-L60).
   - **Impact**: Addressing local arrays/buffers on stack or passing stack addresses to callee functions fails.

---

### 2.2 Sign and Zero Extension (P0)
GCC emits sign/zero extensions frequently for integer promotions in C (`char` and `short` operations).

1. **`SXTH`, `SXTB`, `UXTH`, `UXTB`**:
   - **Encoding**:
     - Pattern: `1011 0010 op2 Rm Rd` (`0xB200` - `0xB2FF`).
     - `SXTH`: `op2 = 00` (`0xB200` | (Rm << 3) | Rd).
     - `SXTB`: `op2 = 01` (`0xB240` | (Rm << 3) | Rd).
     - `UXTH`: `op2 = 10` (`0xB280` | (Rm << 3) | Rd).
     - `UXTB`: `op2 = 11` (`0xB2C0` | (Rm << 3) | Rd).
   - **Codebase Status**:
     - Missing from [instruction.hpp](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/instruction.hpp) (no `SXTH`, `SXTB`, `UXTH`, `UXTB` enum values).
     - Missing from [decoder.hpp](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp) (`0xB200`-`0xB2FF` unhandled).
     - Missing from [interpreter.hpp](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/interpreter.hpp).
   - **Impact**: String operations, byte/halfword cast loops, and peripheral register bitfield masking fault immediately.

---

### 2.3 Branching Extensions: 32-bit Branch `B.W` (P0)
Standard Cortex-M3 binaries generated with `-O2` frequently branch across large functions or sections.

1. **32-bit Conditional and Unconditional Branches (`B.W`)**:
   - **Encoding**:
     - Conditional `B<c>.W`: `w1 = 1111 0 S cond imm6`, `w2 = 10 J1 0 J2 imm11` (`0xF000` / `0x8000`). Range $\pm 1\text{MB}$.
     - Unconditional `B.W`: `w1 = 1111 0 S imm10`, `w2 = 10 J1 1 J2 imm11` (`0xF000` / `0x9000`). Range $\pm 16\text{MB}$.
   - **Codebase Status**:
     - [decoder.hpp#L513-L534](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp#L513-L534) decodes `BL` and `BLX` (`w2 & 0xD000 == 0xD000`), but completely misses `B.W` (`w2 & 0xD000 == 0x9000` or `w2 & 0xD000 == 0x8000`).
   - **Impact**: Any jump exceeding 2 KB (or 256 B for conditional jumps) crashes as `UndefinedInstructionException`.

---

### 2.4 Byte Reversal & Bit Manipulation (P1)
1. **`REV`, `REV16`, `REVSH`**:
   - **Encoding**:
     - Pattern: `1011 1010 op2 Rm Rd` (`0xBA00` - `0xBAFF`).
     - `REV`: `op2 = 00` (`0xBA00`).
     - `REV16`: `op2 = 01` (`0xBA40`).
     - `REVSH`: `op2 = 11` (`0xBAC0`).
   - **Codebase Status**: Missing from enum, decoder, interpreter.
   - **Impact**: Endianness conversion functions (`htons`, `htonl`, `ntohs`) and network protocol parsers fault.

---

### 2.5 32-bit Hardware Division: `UDIV` and `SDIV` (P1)
Cortex-M3 provides native 32-bit hardware divide. GCC generates `udiv` and `sdiv` rather than calling `__aeabi_uidiv` helper functions.

1. **Encoding**:
   - `UDIV Rd, Rn, Rm`: `w1 = 1111 1011 1011 Rn` (`0xFBB0`), `w2 = 1111 Rd 1111 Rm` (`0xF0F0`).
   - `SDIV Rd, Rn, Rm`: `w1 = 1111 1011 1001 Rn` (`0xFB90`), `w2 = 1111 Rd 1111 Rm` (`0xF0F0`).
2. **Codebase Status**:
   - Missing from [instruction.hpp](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/instruction.hpp).
   - [decoder.hpp#L483-L498](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp#L483-L498) only handles `MUL` and `MLA` (`0xFB00`).
   - Missing in [interpreter.hpp](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/interpreter.hpp). (Division by zero behavior: returns 0 or triggers UsageFault depending on CCR.DIV_0_TRP).

---

### 2.6 Extended 32-bit Load / Store Variants (P1)
1. **`LDR.W` / `STR.W` with Negative or Shifted Offsets**:
   - **Current Code**:
     - [decoder.hpp#L536-L560](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp#L536-L560) only decodes 12-bit unsigned immediate offsets (`w1 & 0xFE00 == 0xF800` where `imm12 = w2 & 0xFFF`).
   - **Missing Encodings**:
     - Negative offsets / pre/post indexing: `w1 = 0xF840 - 0xF85F` (bit [11] of w2 indicates addition/subtraction or index mode).
     - Register offset with shift: `w1 = 0xF810 - 0xF830`, `w2 = [Rm, shift]`.
     - PC-relative 32-bit literal load: `LDR.W Rd, [PC, #+/-imm12]` (`0xF85F`).
   - **Impact**: Negative indexing in loops, pointer decrementing, and large constant literal pools fail.

---

### 2.7 32-bit Stack & Multiple Register Operations: `PUSH.W` / `POP.W` (P1)
1. **Encodings**:
   - `STMDB.W / PUSH.W`: `1110 1001 00x0 Rn` (`0xE92D` for SP). Supports high registers `r8-r12, lr`.
   - `LDMIA.W / POP.W`: `1110 1000 10x1 Rn` (`0xE8BD` for SP). Supports high registers and `pc`.
2. **Codebase Status**:
   - Thumb-1 PUSH/POP in [decoder.hpp#L286-L301](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp#L286-L301) only supports `r0-r7` + `lr/pc`.
   - When functions use 5+ local variables or register spilling to `r8-r11`, GCC emits `PUSH.W {r4, r5, r6, r7, r8, lr}` which triggers an undefined instruction fault.

---

### 2.8 System Barriers and Hints (P2)
1. **Barriers (`DMB`, `DSB`, `ISB`)**:
   - `DMB`: `0xF3BF 0x8F50`
   - `DSB`: `0xF3BF 0x8F40`
   - `ISB`: `0xF3BF 0x8F60`
2. **Interrupt Controls & Hints**:
   - `CPSID i` (`0xB672`), `CPSIE i` (`0xB662`).
   - `WFI` (`0xBF30`), `WFE` (`0xBF20`), `SEV` (`0xBF40`).
   - Currently, [decoder.hpp#L330-L334](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/decoder.hpp#L330-L334) only decodes `NOP` (`0xBF00`). All other hints fault.
3. **If-Then (IT) Blocks**:
   - `IT<x><y><z> cond`: `0xBF00 | (cond << 4) | mask`.
   - In Thumb-2, conditional execution of 16-bit ALU instructions is enabled via IT blocks. In TinyCpuSim, every instruction has `cond = ConditionCode::AL` unless set by 16-bit conditional branch.

---

## 3. Loader & Startup Audit

### 3.1 Initial MSP and Vector Table
In ARM Cortex-M architecture:
1. Upon de-asserting reset, the hardware core reads the word at address `0x00000000` (or `VTOR + 0x00`) and places it into `SP` (Main Stack Pointer, MSP).
2. The core then reads the word at address `0x00000004` (or `VTOR + 0x04`) and places it into `PC` (Reset Handler address).
3. The LSB of the Reset Handler address is used to set `CPSR.T` (must be 1 for Thumb mode).

**TinyCpuSim Reality**:
- `ArchitecturalState::reset()` ([state.hpp#L28-L31](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/state.hpp#L28-L31)):
  ```cpp
  registers_.fill(0);
  cpsr_ = 0;
  ```
- `Loader::load_elf()` ([loader.hpp#L102-L104](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/loader.hpp#L102-L104)):
  ```cpp
  // Set entry point (mask out bit 0 for Thumb entry addresses)
  state.set_pc(e_entry & ~1u);
  ```
  `Loader` never reads vector 0 from memory to set SP!
- In `firmware/bsp/startup.s` ([startup.s#L34-L35](file:///home/kw/workspace/TinyCpuSim/.worker-tree/firmware/bsp/startup.s#L34-L35)):
  ```s
  Reset_Handler:
      ldr sp, =_stack_top
  ```
  The firmware authors had to add manual `ldr sp, =_stack_top` in assembly as a workaround for the loader's lack of hardware MSP initialization.

### 3.2 C Runtime Initialization (.data & .bss)
In standard C embedded applications:
1. Non-zero initialized global variables (`.data`) reside in non-volatile flash at load-memory address (`_etext`) and must be copied to run-memory address (`_sdata` through `_edata`) before calling `main`.
2. Zero-initialized global variables (`.bss`) must be cleared to zero (`_sbss` through `_ebss`).

**TinyCpuSim Reality**:
- In `firmware/bsp/startup.s` ([startup.s#L34-L40](file:///home/kw/workspace/TinyCpuSim/.worker-tree/firmware/bsp/startup.s#L34-L40)):
  ```s
  Reset_Handler:
      ldr sp, =_stack_top
      bl main
      svc #0
      b .
  ```
  There is **no** loop copying `.data` and **no** loop zeroing `.bss`.
- `Loader::load_elf()` in [loader.hpp#L96-L98](file:///home/kw/workspace/TinyCpuSim/.worker-tree/include/tinyarmsim/loader.hpp#L96-L98):
  ```cpp
  // Zero-fill remaining BSS (p_memsz - p_filesz)
  for (uint32_t b = p_filesz; b < p_memsz; ++b) {
      bus.write8(p_vaddr + b, 0);
  }
  ```
  `Loader::load_elf` only zeroes out the tail of a PT_LOAD segment if memsz > filesz in that specific segment. In `firmware/bsp/linker.ld`, `.data` has `AT (_etext)`, meaning in a multi-segment ELF or ROM image, `.data` remains in FLASH at `_etext` and is never moved to RAM at `0x00020000`. C code accessing global initialized variables will read garbage uninitialized RAM.

---

## 4. Implementation Recommendations & Road Map

### Phase 1: Minimal Functional Bare-Metal C (Est. 1 PR)
1. Add `Opcode::SXTB`, `Opcode::SXTH`, `Opcode::UXTB`, `Opcode::UXTH`.
2. Add decoding for:
   - `0xB000` / `0xB080`: `ADD/SUB sp, sp, #imm7`.
   - `0xA000` / `0xA800`: `ADR Rd, #imm8` / `ADD Rd, SP, #imm8`.
   - `0xB200` - `0xB2FF`: `SXTH`, `SXTB`, `UXTH`, `UXTB`.
   - `0xF000 0x8000` / `0x9000`: `B.W`.
   - `0xBF20` - `0xBF40`: `WFI`, `WFE`, `SEV` (decode as NOP/hint).
   - `0xB662` / `0xB672`: `CPSIE`, `CPSID` (decode as NOP or toggle PRIMASK).
   - `0xF3BF 0x8F40-0x8F60`: `DMB`, `DSB`, `ISB` (decode as NOP barrier).
3. Update `firmware/bsp/startup.s` to implement `.data` copy from `_etext` to `_sdata` and `.bss` zeroing from `_sbss` to `_ebss`.
4. Update `Loader::load_elf` or CPU reset sequence to read `state.set_sp(bus.read32(vector_base))` on reset.

### Phase 2: Complete Thumb-2 Instruction Set (Est. 2 PRs)
1. Add `UDIV` / `SDIV` (`0xFB90` / `0xFBB0`).
2. Add `REV`, `REV16`, `REVSH` (`0xBA00`).
3. Add full 32-bit `PUSH.W` / `POP.W` (`0xE92D` / `0xE8BD`).
4. Add 32-bit `LDR.W` / `STR.W` register-offset and negative immediate decoding.
5. Add IT block state tracking in `ArchitecturalState`.
