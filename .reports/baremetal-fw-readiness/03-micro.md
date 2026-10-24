# Micro skeletons and source slices

Extractor requested by explore skill is not present at `scripts/extract_skeleton.sh` in this worker checkout. The following bounded declaration/call skeletons were read directly; slices are 10 lines each.

## CLI path — `src/main.cpp`

```cpp
MemoryBus bus(64 * 1024 * 1024);
ArchitecturalState state;
IsaInterpreter interpreter(state, bus);
Loader::load_elf(file, bus, state); // Default mode
exit_code = interpreter.run(max_steps);
```

`src/main.cpp:242-251`:
```text
242 MemoryBus bus(64 * 1024 * 1024);
243 ArchitecturalState state;
244 IsaInterpreter interpreter(state, bus);
245 interpreter.set_logging(enable_log);
246
247 SliceManager slice_manager(slice_cfg);
248 if (!enable_uarch && slice_cfg.is_active()) {
249     interpreter.set_slice_manager(&slice_manager);
250 }
251
```

`src/main.cpp:265-274`:
```text
265 try {
266     Loader::load_elf(file, bus, state);
267 } catch (const std::exception& e) {
268     ...
269     return 1;
270 }
271
272 std::cout << "Entry Point: " << state.get_pc();
273            << "Starting simulation...";
274
```

## Loader and board seam

```cpp
enum class ElfLoadMode { Default, Firmware };
load_elf(stream, bus, state, mode);
load_address = mode == Firmware ? p_paddr : p_vaddr;
boot_cortex_m(bus, state, vtor_base); // MSP <- VTOR, PC <- VTOR+4
SoCPeripherals::attach_all(bus, interp); // register devices + attach NVIC
```

`include/tinyarmsim/loader.hpp:93-102`:
```text
93 if (p_type == 1) {
94     if (p_offset + p_filesz > data.size()) throw ...;
95
96     const uint32_t load_address = mode == ElfLoadMode::Firmware ? p_paddr : p_vaddr;
97     for (uint32_t b = 0; b < p_filesz; ++b) {
98         bus.write8(load_address + b, data[p_offset + b]);
99     }
100
101    if (mode == ElfLoadMode::Default || load_address == p_vaddr) {
102        for (uint32_t b = p_filesz; b < p_memsz; ++b) {
```

`include/tinyarmsim/loader.hpp:115-121`:
```text
115 // Cortex-M hardware reset sequence ...
116 static void boot_cortex_m(const MemoryBus& bus, ArchitecturalState& state, uint32_t vtor_base = 0) {
117     uint32_t initial_msp = bus.read32(vtor_base);
118     uint32_t reset_vector = bus.read32(vtor_base + 4u);
119     state.set_sp(initial_msp);
120     state.set_pc(reset_vector & ~1u);
121 }
```

`include/tinyarmsim/mmio/soc_platform.hpp:52-63`:
```text
52 bus.register_device(std::move(uart_dev));
53 bus.register_device(std::move(timer_dev));
54 bus.register_device(std::move(i2c_dev));
55 bus.register_device(std::move(spi_dev));
56 bus.register_device(std::move(crc_dev));
57 bus.register_device(std::move(gpio_dev));
58 bus.register_device(std::move(dma_dev));
59 bus.register_device(std::move(custom_dev));
60 bus.register_device(std::move(nvic_dev));
61
62 interp.attach_nvic(p.nvic);
63 return p;
```

## Acceptance harness skeleton

```cpp
MemoryBus bus(64 * 1024 * 1024);
ArchitecturalState state;
IsaInterpreter interp(state, bus);
auto soc = SoCPeripherals::attach_all(bus, interp);
Loader::load_elf(..., ElfLoadMode::Firmware);
Loader::boot_cortex_m(..., VTOR);
ASSERT/EXPECT data, BSS, result markers and clean SVC exit;
```

`tests/baremetal_firmware_test.cpp:367-376`:
```text
367 TEST(...CompiledCortexMBootInitializesDataAndBssAndExits) {
368     MemoryBus bus(64 * 1024 * 1024);
369     ArchitecturalState state;
370     IsaInterpreter interp(state, bus);
371
372     auto soc = mmio::SoCPeripherals::attach_all(bus, interp);
373     std::ifstream elf(BAREMETAL_ACCEPTANCE_ELF, std::ios::binary);
374     ASSERT_TRUE(elf.good());
375     const std::vector<uint8_t> elf_bytes(...);
376     ASSERT_GE(elf_bytes.size(), 52u);
```

`tests/baremetal_firmware_test.cpp:444-453`:
```text
444 const uint32_t initialized_address = symbol_address("initialized_value");
445 const uint32_t zero_address = symbol_address("zero_value");
446 const uint32_t acceptance_address = symbol_address("acceptance_result");
447 const uint32_t results_address = symbol_address("acceptance_results");
448 const uint32_t marker_address = symbol_address("isa_address_marker");
449 EXPECT_TRUE(symbol_tables_valid);
450 ASSERT_NE(initialized_address, 0u);
451 ASSERT_NE(zero_address, 0u);
452 ASSERT_NE(acceptance_address, 0u);
453 ASSERT_NE(results_address, 0u);
```

## BSP startup and memory map

`firmware/bsp/startup.s:31-40`:
```text
31 .section .text.Reset_Handler, "ax", %progbits
32 .global Reset_Handler
33 .type Reset_Handler, %function
34 Reset_Handler:
35     ldr sp, =_stack_top
36
37     /* Copy .data section from FLASH (_etext) to RAM (_sdata .. _edata) */
38     ldr r0, =_sdata
39     ldr r1, =_edata
40     ldr r2, =_etext
```

`firmware/bsp/linker.ld:3-12`:
```text
3 MEMORY
4 {
5     FLASH (rx)  : ORIGIN = 0x00010000, LENGTH = 256K
6     RAM   (rwx) : ORIGIN = 0x00020000, LENGTH = 64K
7 }
8
9 _stack_top = ORIGIN(RAM) + LENGTH(RAM);
10
11 SECTIONS
12 {
```
