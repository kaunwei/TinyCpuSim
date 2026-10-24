# Reproducible source and tool settings

All source excerpts below were read from the task probe files in `/tmp`; the repository itself was not modified for these probes.

## Natural C functions (`/tmp/task051-reliable/probes.c`)

```c
__attribute__((noinline)) unsigned probe_select(unsigned x) { return x ? 7u : 9u; }
__attribute__((noinline)) int probe_negate(int x) { return -x; }
__attribute__((noinline)) unsigned probe_compare(int x) { return x == -128; }
```

Validated reliable machine-readable outcomes: `/tmp/task051-reliable/results.json` (12 rows). Compiler: `arm-none-eabi-gcc 13.2.1`; target `-mcpu=cortex-m3 -mthumb`; optimization variants `-O0`, `-O2`; no no-if-conversion flags. These are standard optimization probe modes, distinct from CMake acceptance fixture flags `-O1 -fno-if-conversion -fno-if-conversion2`.

## Direct function-call harness (`/tmp/task051-reliable/direct.cpp`)

```cpp
MemoryBus bus(64u*1024u*1024u); ArchitecturalState s; IsaInterpreter cpu(s,bus);
std::ifstream f(argv[1],std::ios::binary);
Loader::load_elf(f,bus,s,Loader::ElfLoadMode::Firmware);
auto entry=static_cast<uint32_t>(std::stoul(argv[2],nullptr,0));
auto input=static_cast<uint32_t>(std::stoul(argv[3],nullptr,0));
s.set_pc(entry&~1u); s.set_sp(0x30000); s.set_lr(0x8001); s.set_reg(0,input);
try { for(unsigned n=0;n<200 && s.get_pc()!=0x8000;++n) cpu.step(); ... }
catch(const std::exception&e) { auto pc=s.get_pc(); ... bus.read16(pc); bus.read16(pc+2); }
```

Input is installed in R0 after loading the ELF/function. Return is detected at sentinel `0x8000`; SP and LR are explicit. This avoids relying on C startup to preserve the probe input.

## Firmware BSP board runner (`/tmp/task051-reliable/fw-runner.cpp`)

```cpp
MemoryBus bus(64u*1024u*1024u); ArchitecturalState state; IsaInterpreter interp(state,bus);
auto soc=mmio::SoCPeripherals::attach_all(bus,interp); soc.nvic->set_vtor(0x10000u);
std::ifstream elf(argv[1],std::ios::binary);
Loader::load_elf(elf,bus,state,Loader::ElfLoadMode::Firmware);
Loader::boot_cortex_m(bus,state,0x10000u);
state.set_reg(0,static_cast<uint32_t>(std::stoul(argv[2],nullptr,0)));
try { auto rc=interp.run(50000); ... }
catch(const std::exception& e) { auto pc=state.get_pc(); ... }
```

This runner was executed on `/tmp/task051/demo.elf`; it faults at actual PC `0x101f8` on `F04F 4280` (full outcome in `evidence.md`). Natural-function semantic probes used the direct-call harness above.

## Full acceptance recipe

CMake compiles `firmware/bsp/startup.s`, `firmware/apps/isa_boot_acceptance.c`, and `firmware/apps/isa_boot_acceptance.s` using the installed ARM compiler, linker script `firmware/bsp/linker.ld`, and fixture flags shown at `CMakeLists.txt:268-280`. Fixture-specific `-O1 -fno-if-conversion -fno-if-conversion2` are intentionally not used for the natural C function probes. Acceptance command and result are in `evidence.md`.
