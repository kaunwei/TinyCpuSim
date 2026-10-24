# Micro skeletons and slices

## Decoder: `include/tinyarmsim/decoder.hpp`

```text
Decoder::decode32(w1, w2, pc)
  T3 conditional branch match: (w1 & F800)==F000 && (w2 & D000)==8000
  cond = (w1 >> 6) & F
  S/J2/J1/imm6/imm11 -> signed displacement
  pc == 0: is_relative=true, imm=displacement
  pc != 0: is_relative=false, imm=pc+4+displacement
```

Slice `decoder.hpp#L670-L686`:

```cpp
// 5.2 32-bit Conditional Branch (B<cond>.W):
if ((w1 & 0xF800) == 0xF000 && (w2 & 0xD000) == 0x8000) {
    uint8_t cond_val = static_cast<uint8_t>((w1 >> 6) & 0xF);
    if (cond_val < 0xE) {
        instr.op = Opcode::B;
        instr.cond = static_cast<ConditionCode>(cond_val);
        uint32_t s = (w1 >> 10) & 0x1;
        uint32_t imm6 = w1 & 0x3F;
```

## Interpreter: `include/tinyarmsim/interpreter.hpp`

```text
IsaInterpreter::execute(instr)
  evaluate_condition reads ArchitecturalState NZCV
  failed condition -> advance_pc(instr.instr_size), return
  Opcode::B -> relative ? current_pc+4+imm : imm
```

Slices: `interpreter.hpp#L67-L91` maps EQ/NE and remaining conditions; `#L108-L111` advances untaken instructions by their size; `#L422-L426` writes branch target.

## State: `include/tinyarmsim/state.hpp`

```text
ArchitecturalState::get_flag_{n,z,c,v} reads CPSR bits
set_flag_{n,z,c,v} changes individual CPSR bits
```

Slice `state.hpp#L63-L72` contains the flag accessors used by condition evaluation.
