# Micro interface skeletons and verified slices

Generated with `scripts/extract_skeleton.sh` (`NIGHTSHIFT_WORKER=1`) against the examined worker tree. Extractor emits declaration skeletons only for recognized language keywords; C++ method signatures below are manually verified source declarations.

## Opcode cache

```cpp
class OpcodeCache {
  const DecodedInstruction& get_16(uint16_t raw_instr);
  const DecodedInstruction& get_32(uint16_t w1, uint16_t w2);
  void clear();
};
```

`include/tinyarmsim/opcode_cache.hpp#L11` (10-line extractor slice):

```text
 9 namespace tinyarmsim {
10
11 class OpcodeCache {
12 public:
13     OpcodeCache() {
14         valid_16_.fill(false);
15     }
16
17     [[nodiscard]] const DecodedInstruction& get_16(uint16_t raw_instr) {
18         if (!valid_16_[raw_instr]) {
19             table_16_[raw_instr] = Decoder::decode16(raw_instr);
```

## Trace seam

```cpp
struct TraceRecord {
  void clear();
  std::string format() const;
};
```

`include/tinyarmsim/trace.hpp#L24`: record holds core/PC/raw encoding/disassembly, register and memory diffs, and NZCV; `format()` spans `#L42-82`. Registered format checks: `tests/trace_test.cpp#L9-43`.

## Multi-core seam

```cpp
class MultiCoreSystem {
  void set_entry_pc(size_t core_id, uint32_t pc);
  void tick();
  uint64_t run(uint64_t max_cycles = 10000000);
  UArchStats collect_stats() const;
};
```

`include/tinyarmsim/uarch/multicore_system.hpp#L19` (10-line extractor slice):

```text
17 namespace tinyarmsim::uarch {
18
19 class MultiCoreSystem {
20 public:
21     explicit MultiCoreSystem(const UArchConfig& config, size_t ram_size = 64 * 1024 * 1024)
22         : config_(config),
23           mem_bus_(ram_size),
24           mem_hierarchy_(mem_bus_, config) {
25         config_.validate();
26         init_cores();
```

## MMIO seam

```cpp
class MMIODevice {
  virtual uint32_t base_address() const noexcept = 0;
  virtual uint32_t address_span() const noexcept = 0;
  virtual uint32_t read(uint32_t offset, AccessSize size) = 0;
  virtual void write(uint32_t offset, uint32_t value, AccessSize size) = 0;
};
```

`include/tinyarmsim/mmio/device.hpp#L14-30`; shared clocked behavior is `ClockedMMIODevice#L33-79`. `SoCPeripherals::attach_all(MemoryBus&, IsaInterpreter&)` registers devices and attaches NVIC (`soc_platform.hpp#L32-65`).

## ISA decode slice

`Decoder::decode32_internal` handles UDIV/SDIV at `include/tinyarmsim/decoder.hpp#L590-598`. 10-line extractor slice:

```text
588 }
589
590 // 3.5 32-bit Hardware Divide (SDIV / UDIV):
591 if (((w1 & 0xFFF0) == 0xFB90 || (w1 & 0xFFF0) == 0xFBB0) && (w2 & 0xF0F0) == 0xF0F0) {
592     bool is_unsigned = ((w1 >> 5) & 0x1) != 0;
593     instr.op = is_unsigned ? Opcode::UDIV : Opcode::SDIV;
594     instr.rn = static_cast<uint8_t>(w1 & 0xF);
595     instr.rd = static_cast<uint8_t>((w2 >> 8) & 0xF);
596     instr.rm = static_cast<uint8_t>(w2 & 0xF);
597     return instr;
598 }
599
```
