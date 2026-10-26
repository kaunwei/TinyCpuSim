# TinyArmSim

TinyArmSim is a modular, high-extensibility ARM CPU simulator written in C++20, starting with an ISA functional interpreter and designed to scale to cycle-accurate Out-of-Order (OoO) and JIT execution engines.

## Language

### Core Architecture & Engines

**ISA Interpreter**:
A functional execution engine that executes ARM/Thumb instructions sequentially without microarchitectural timing.
_Avoid_: Emulator, VM

**Cycle Engine**:
A microarchitectural simulation engine modeling timing, pipelining, Out-of-Order (OoO) execution, and branch speculation.
_Avoid_: Hardware emulator, clock ticker

**JIT Engine**:
A dynamic binary translation engine compiling guest ARM instructions into host machine code for high-throughput execution.
_Avoid_: Ahead-of-time compiler

**Architectural State**:
The definitive register file (R0-R15, CPSR/NZCV) representing committed CPU state, isolated from microarchitectural speculative state.
_Avoid_: CPU globals, raw context

**Decoded Instruction**:
A structured, engine-agnostic representation of an unpacked ARM/Thumb instruction containing opcode, operands, shift rules, and flags.
_Avoid_: Micro-op callback, opcode function pointer

**Memory Bus**:
A flat 64MB physical address space interconnect delivering byte/halfword/word access without virtual memory translation or dynamic linking.
_Avoid_: Virtual memory, page table, RAM array

**CPU Fault**:
A typed, deterministic exception raised upon illegal instruction decoding, unaligned memory access, or bus boundary violations.
_Avoid_: Crash, panic, abort

### Verification & Loading

**Loader**:
A dual-mode binary loader supporting statically-linked ELF32 executables and raw flat binaries with no dynamic library overhead.
_Avoid_: OS dynamic linker, file reader

**Self-Test Assembly**:
Cross-compiled ARM assembly test programs executing in flat memory and terminating via SVC/SWI with an exit status in R0.
_Avoid_: Unit test scripts, mock programs

**Mispredict Flush**:
The recovery protocol in the Cycle Engine that squashes speculative instructions from the pipeline and Reorder Buffer (ROB) upon branch prediction failure.
_Avoid_: Reset, rollback
