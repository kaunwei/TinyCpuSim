<!-- REPORT_META: Task=TASK-051 | Commit=6f0f42e58686fd43c5c7ab7b0192121b02b965de -->
# Actual helper extraction and exact ten-line slices

The human helper exists and was executed as the research worker against current worker source, with `NIGHTSHIFT_WORKER=1`. Its declaration extraction is regex-based, not a full C++ parser. Output below is verbatim; separate slices are exactly10source lines each.

Command: `NIGHTSHIFT_WORKER=1 bash /home/kw/workspace/TinyCpuSim/scripts/extract_skeleton.sh include/tinyarmsim/decoder.hpp` (cwd .worker-tree).

### Interface Skeleton: `include/tinyarmsim/decoder.hpp`
```
class Decoder 
```

Command: `NIGHTSHIFT_WORKER=1 bash /home/kw/workspace/TinyCpuSim/scripts/extract_skeleton.sh include/tinyarmsim/instruction.hpp` (cwd .worker-tree).

### Interface Skeleton: `include/tinyarmsim/instruction.hpp`
```
enum class Opcode 
enum class ConditionCode : uint8_t 
enum class ShiftType : uint8_t 
struct DecodedInstruction 
```

Command: `NIGHTSHIFT_WORKER=1 bash /home/kw/workspace/TinyCpuSim/scripts/extract_skeleton.sh include/tinyarmsim/interpreter.hpp` (cwd .worker-tree).

### Interface Skeleton: `include/tinyarmsim/interpreter.hpp`
```
struct SimulationStats 
class IsaInterpreter 
```

Command: `NIGHTSHIFT_WORKER=1 bash /home/kw/workspace/TinyCpuSim/scripts/extract_skeleton.sh include/tinyarmsim/state.hpp` (cwd .worker-tree).

### Interface Skeleton: `include/tinyarmsim/state.hpp`
```
class ArchitecturalState 
```

Command: `NIGHTSHIFT_WORKER=1 bash /home/kw/workspace/TinyCpuSim/scripts/extract_skeleton.sh include/tinyarmsim/loader.hpp` (cwd .worker-tree).

### Interface Skeleton: `include/tinyarmsim/loader.hpp`
```
class LoaderException : public std::runtime_error 
class Loader 
    enum class ElfLoadMode { Default, Firmware };
```

Command: `NIGHTSHIFT_WORKER=1 bash /home/kw/workspace/TinyCpuSim/scripts/extract_skeleton.sh include/tinyarmsim/mmio/soc_platform.hpp` (cwd .worker-tree).

### Interface Skeleton: `include/tinyarmsim/mmio/soc_platform.hpp`
```
struct SoCPeripherals 
```

## src/main.cpp#L242 (ten source lines)

```text
242:     tinyarmsim::MemoryBus bus(64 * 1024 * 1024); // 64MB RAM
243:     tinyarmsim::ArchitecturalState state;
244:     tinyarmsim::IsaInterpreter interpreter(state, bus);
245:     interpreter.set_logging(enable_log);
246: 
247:     tinyarmsim::uarch::SliceManager slice_manager(slice_cfg);
248:     if (!enable_uarch && slice_cfg.is_active()) {
249:         interpreter.set_slice_manager(&slice_manager);
250:     }
251: 
```

## src/main.cpp#L262 (ten source lines)

```text
262:                   << "\n";
263:     }
264: 
265:     try {
266:         tinyarmsim::Loader::load_elf(file, bus, state);
267:     } catch (const std::exception& e) {
268:         std::cerr << "Error loading ELF: " << e.what() << "\n";
269:         print_banner(false, 1, e.what(), interpreter.get_stats());
270:         return 1;
271:     }
```

## include/tinyarmsim/decoder.hpp#L130 (ten source lines)

```text
130:             instr.rd = static_cast<uint8_t>(raw_instr & 0x7);
131:             instr.rn = instr.rd;
132:             instr.rm = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
133:             instr.is_imm = false;
134:             instr.set_flags = true;
135: 
136:             switch (alu_op) {
137:                 case 0b0000: instr.op = Opcode::AND; break;
138:                 case 0b0001: instr.op = Opcode::EOR; break;
139:                 case 0b0010: instr.op = Opcode::LSL; break;
```

## include/tinyarmsim/decoder.hpp#L338 (ten source lines)

```text
338:         // 14.1 System Hints: WFE (0xBF20), WFI (0xBF30), SEV (0xBF40), IT (0xBF01-0xBFEF)
339:         if ((raw_instr & 0xFF00) == 0xBF00) {
340:             if (raw_instr == 0xBF30 || raw_instr == 0xBF20) {
341:                 instr.op = Opcode::WFI;
342:             } else {
343:                 instr.op = Opcode::NOP;
344:             }
345:             return instr;
346:         }
347: 
```

## include/tinyarmsim/interpreter.hpp#L270 (ten source lines)

```text
270:             case Opcode::RSB: {
271:                 uint32_t op1 = state_.get_reg(instr.rn);
272:                 uint32_t op2 = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
273:                 uint64_t result = static_cast<uint64_t>(op2) - static_cast<uint64_t>(op1);
274:                 uint32_t res32 = static_cast<uint32_t>(result);
275: 
276:                 write_reg(instr.rd, res32);
277: 
278:                 if (instr.set_flags) {
279:                     bool n = (res32 & 0x80000000u) != 0;
```

## include/tinyarmsim/loader.hpp#L94 (ten source lines)

```text
94:                 if (p_offset + p_filesz > data.size()) {
95:                     throw LoaderException("PT_LOAD segment data exceeds file size");
96:                 }
97: 
98:                 const uint32_t load_address = mode == ElfLoadMode::Firmware ? p_paddr : p_vaddr;
99:                 for (uint32_t b = 0; b < p_filesz; ++b) {
100:                     bus.write8(load_address + b, data[p_offset + b]);
101:                 }
102: 
103:                 if (mode == ElfLoadMode::Default || load_address == p_vaddr) {
```

## include/tinyarmsim/loader.hpp#L112 (ten source lines)

```text
112:         state.set_pc(e_entry & ~1u);
113:     }
114: 
115:     // Cortex-M hardware reset sequence: loads initial MSP from VTOR+0 and Reset_Handler PC from VTOR+4
116:     static void boot_cortex_m(const MemoryBus& bus, ArchitecturalState& state, uint32_t vtor_base = 0x00000000) {
117:         uint32_t initial_msp = bus.read32(vtor_base);
118:         uint32_t reset_vector = bus.read32(vtor_base + 4u);
119:         state.set_sp(initial_msp);
120:         state.set_pc(reset_vector & ~1u);
121:     }
```

## include/tinyarmsim/mmio/soc_platform.hpp#L52 (ten source lines)

```text
52:         bus.register_device(std::move(uart_dev));
53:         bus.register_device(std::move(timer_dev));
54:         bus.register_device(std::move(i2c_dev));
55:         bus.register_device(std::move(spi_dev));
56:         bus.register_device(std::move(crc_dev));
57:         bus.register_device(std::move(gpio_dev));
58:         bus.register_device(std::move(dma_dev));
59:         bus.register_device(std::move(custom_dev));
60:         bus.register_device(std::move(nvic_dev));
61: 
```

## tests/baremetal_firmware_test.cpp#L367 (ten source lines)

```text
367: TEST(BaremetalFirmwareTest, CompiledCortexMBootInitializesDataAndBssAndExits) {
368:     MemoryBus bus(64 * 1024 * 1024);
369:     ArchitecturalState state;
370:     IsaInterpreter interp(state, bus);
371: 
372:     auto soc = mmio::SoCPeripherals::attach_all(bus, interp);
373:     std::ifstream elf(BAREMETAL_ACCEPTANCE_ELF, std::ios::binary);
374:     ASSERT_TRUE(elf.good());
375:     const std::vector<uint8_t> elf_bytes((std::istreambuf_iterator<char>(elf)),
376:                                          std::istreambuf_iterator<char>());
```

## firmware/bsp/startup.s#L34 (ten source lines)

```text
34: Reset_Handler:
35:     ldr sp, =_stack_top
36: 
37:     /* Copy .data section from FLASH (_etext) to RAM (_sdata .. _edata) */
38:     ldr r0, =_sdata
39:     ldr r1, =_edata
40:     ldr r2, =_etext
41: 1:
42:     cmp r0, r1
43:     bhs 2f
```

