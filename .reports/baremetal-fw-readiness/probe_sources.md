<!-- REPORT_META: Task=TASK-051 | Commit=6f0f42e58686fd43c5c7ab7b0192121b02b965de -->
# Complete reproducible sources and tool settings

Run commands from the human repository root. Compiler: `/usr/bin/arm-none-eabi-gcc`13.2.1. Save each complete source below at its named /tmp/task051-reliable path (create that directory first). Natural probes use ordinary O0/O2 without if-conversion workarounds.

## /tmp/task051-reliable/probes.c

```c
__attribute__((noinline)) unsigned probe_select(unsigned x) { return x ? 7u : 9u; }
__attribute__((noinline)) int probe_negate(int x) { return -x; }
__attribute__((noinline)) unsigned probe_compare(int x) { return x == -128; }
```

## /tmp/task051-reliable/direct.cpp

```cpp
#include <fstream>
#include <iostream>
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/loader.hpp"
int main(int argc,char**argv){
 using namespace tinyarmsim;
 MemoryBus bus(64u*1024u*1024u);ArchitecturalState s;IsaInterpreter cpu(s,bus);
 std::ifstream f(argv[1],std::ios::binary);Loader::load_elf(f,bus,s,Loader::ElfLoadMode::Firmware);
 auto entry=static_cast<uint32_t>(std::stoul(argv[2],nullptr,0));auto input=static_cast<uint32_t>(std::stoul(argv[3],nullptr,0));
 s.set_pc(entry&~1u);s.set_sp(0x30000);s.set_lr(0x8001);s.set_reg(0,input);
 try {unsigned n=0;for(;n<200 && s.get_pc()!=0x8000;++n)cpu.step();
  std::cout<<"returned="<<(s.get_pc()==0x8000)<<" input="<<std::hex<<input<<" actual="<<s.get_reg(0)<<" sp="<<s.get_sp()<<" steps="<<std::dec<<n<<"\n";return s.get_pc()==0x8000?0:3;}
 catch(const std::exception&e){auto pc=s.get_pc();std::cout<<"fault_pc="<<std::hex<<pc<<" raw1="<<bus.read16(pc)<<" raw2="<<bus.read16(pc+2)<<" message="<<e.what()<<"\n";return 2;}
}
```

## /tmp/task051-reliable/fw-runner.cpp

```cpp
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/loader.hpp"
#include "tinyarmsim/mmio/soc_platform.hpp"
using namespace tinyarmsim;
int main(int, char** argv) {
  MemoryBus bus(64u*1024u*1024u); ArchitecturalState state; IsaInterpreter interp(state,bus);
  auto soc=mmio::SoCPeripherals::attach_all(bus,interp); soc.nvic->set_vtor(0x10000u);
  std::ifstream elf(argv[1],std::ios::binary); Loader::load_elf(elf,bus,state,Loader::ElfLoadMode::Firmware); Loader::boot_cortex_m(bus,state,0x10000u);
  state.set_reg(0,static_cast<uint32_t>(std::stoul(argv[2],nullptr,0)));
  try { auto rc=interp.run(50000); std::cout<<"rc="<<rc<<" pc="<<std::hex<<state.get_pc()<<" cond="<<bus.read32(0x20000)<<" neg="<<bus.read32(0x20004)<<" cmp="<<bus.read32(0x20008)<<"\n"; return 0; }
  catch(const std::exception& e){uint32_t pc=state.get_pc(); uint16_t a=bus.read16(pc), b=bus.read16(pc+2); std::cout<<"fault pc="<<std::hex<<pc<<" raw="<<a<<" "<<b<<" msg="<<e.what()<<"\n";return 2;}
}
```

## Exact compilation commands

```bash
/usr/bin/g++ -std=c++20 -I .worker-tree/include /tmp/task051-reliable/direct.cpp -o /tmp/task051-reliable/direct
/usr/bin/g++ -std=c++20 -I .worker-tree/include /tmp/task051-reliable/fw-runner.cpp -o /tmp/task051-reliable/fw-runner
/usr/bin/arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -ffreestanding -fno-builtin -nostdlib -O0 -Wl,-Ttext=0x10000,-e,probe_select /tmp/task051-reliable/probes.c -o /tmp/task051-reliable/O0.elf
/usr/bin/arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -ffreestanding -fno-builtin -nostdlib -O2 -Wl,-Ttext=0x10000,-e,probe_select /tmp/task051-reliable/probes.c -o /tmp/task051-reliable/O2.elf
/usr/bin/arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -O2 -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -I .worker-tree/firmware/bsp -T .worker-tree/firmware/bsp/linker.ld .worker-tree/firmware/bsp/startup.s .worker-tree/firmware/bsp/*.c .worker-tree/firmware/apps/demo_all_peripherals.c -o /tmp/task051-reliable/demo-O2.elf
/tmp/task051-reliable/fw-runner /tmp/task051-reliable/demo-O2.elf 0
/tmp/task051-reliable/fw-runner .worker-tree/build/isa_boot_acceptance.elf 0
```

## Run all12 independent input cases

```bash
/tmp/task051-reliable/direct /tmp/task051-reliable/O0.elf 0x10000 0x0
/tmp/task051-reliable/direct /tmp/task051-reliable/O0.elf 0x10000 0x1
/tmp/task051-reliable/direct /tmp/task051-reliable/O0.elf 0x1001e 0x80
/tmp/task051-reliable/direct /tmp/task051-reliable/O0.elf 0x1001e 0xffffff80
/tmp/task051-reliable/direct /tmp/task051-reliable/O0.elf 0x10034 0xffffff80
/tmp/task051-reliable/direct /tmp/task051-reliable/O0.elf 0x10034 0xffffff81
/tmp/task051-reliable/direct /tmp/task051-reliable/O2.elf 0x10000 0x0
/tmp/task051-reliable/direct /tmp/task051-reliable/O2.elf 0x10000 0x1
/tmp/task051-reliable/direct /tmp/task051-reliable/O2.elf 0x1000c 0x80
/tmp/task051-reliable/direct /tmp/task051-reliable/O2.elf 0x1000c 0xffffff80
/tmp/task051-reliable/direct /tmp/task051-reliable/O2.elf 0x10010 0xffffff80
/tmp/task051-reliable/direct /tmp/task051-reliable/O2.elf 0x10010 0xffffff81
```

Inspect machine code with `/usr/bin/arm-none-eabi-objdump -d /tmp/task051-reliable/O2.elf` and symbols with `/usr/bin/arm-none-eabi-nm -n /tmp/task051-reliable/O2.elf`. Source examples remain actual function symbols and arguments are injected in R0 after loading, not folded into C constants or erased by startup.

The board runner sets SoC, VTOR, Firmware loader and vector reset before run. Its post-boot R0 argument is not a claim that C startup preserves arguments. Its legacy cond/neg/cmp output labels are fixed-address diagnostic words; only rc/fault PC/raw instruction are used for readiness evidence. No BSS addresses in that board runner are used for natural semantic tests.
