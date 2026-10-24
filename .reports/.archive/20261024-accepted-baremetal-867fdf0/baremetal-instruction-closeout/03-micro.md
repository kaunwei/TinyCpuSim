# Interface micro-skeletons and source slices

Generated with the repository helper at `/home/kw/workspace/TinyCpuSim/scripts/extract_skeleton.sh` using `NIGHTSHIFT_WORKER=1`; source is current worker-tree code.

## Decoder

```cpp
class Decoder
```

### `include/tinyarmsim/decoder.hpp#L450`

```cpp
    [[nodiscard]] static DecodedInstruction decode32_internal(uint16_t w1, uint16_t w2, uint32_t pc = 0) {
        DecodedInstruction instr{};
        instr.instr_size = 4;
        instr.cond = ConditionCode::AL;

        // 1. MOVW / MOVT:
        if ((w1 & 0xFB70) == 0xF240) {
            bool is_movt = ((w1 >> 7) & 0x1) != 0;
            instr.op = is_movt ? Opcode::MOVT : Opcode::MOVW;
```

## Interpreter

```cpp
struct SimulationStats
class IsaInterpreter
```

### `include/tinyarmsim/interpreter.hpp#L538`

```cpp
            // === Multiple Load/Store: LDM / STM ===
            case Opcode::STM: {
                uint32_t base = state_.get_reg(instr.rn);
                uint32_t cur_addr = base;
                for (size_t i = 0; i < NUM_REGISTERS; ++i) {
                    if (instr.register_list & (1 << i)) {
                        write_mem(cur_addr, state_.get_reg(i), 4);
                        cur_addr += 4;
                    }
                }
                if (instr.writeback) {
```

## Loader

```cpp
class LoaderException : public std::runtime_error
class Loader
```

### `include/tinyarmsim/loader.hpp#L106`

```cpp
    // Cortex-M hardware reset sequence: loads initial MSP from VTOR+0 and Reset_Handler PC from VTOR+4
    static void boot_cortex_m(const MemoryBus& bus, ArchitecturalState& state, uint32_t vtor_base = 0x00000000) {
        uint32_t initial_msp = bus.read32(vtor_base);
        uint32_t reset_vector = bus.read32(vtor_base + 4u);
        state.set_sp(initial_msp);
        state.set_pc(reset_vector & ~1u);
    }
};

} // namespace tinyarmsim
```

## NVIC

```cpp
class NVICDevice : public MMIODevice
```

### `include/tinyarmsim/mmio/nvic.hpp#L118`

```cpp
    bool poll_and_try_enter_exception(ArchitecturalState& state, MemoryBus& bus) {
        for (const auto& slot : bus.mmio_devices()) {
            if (slot.dev && slot.dev.get() != this && slot.dev->has_pending_irq()) {
                set_pending(slot.dev->irq_line());
            }
        }
        if (state.get_primask()) return false; // Masked by CPSID i
        int irq = get_highest_pending_irq();
```
