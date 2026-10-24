<!-- REPORT_META: Task=TASK-045 | Commit=68ba31d06b3c11a091f8fc71fc5d8d7bfe1032ba -->
# T3 conditional B.W acceptance probe

## Finding
The original-contract conditional wide-branch acceptance uses valid instructions only if assembler output matches the actual testcase halfwords and PCs. Independent Cortex-M3 assembly and a current-header execution probe show T3 B.W handles forward conditional branches, condition gating, and negative offsets correctly in the tested BEQ/BNE cases. No decoder, condition-evaluation, or PC-offset defect reproduced. The earlier failing acceptance therefore points to a test-word/PC/expected-target mismatch; see exact artifacts and expected/observed PCs in [evidence.md](evidence.md).

## Topology and coupling
`Decoder::decode32` converts T3 fields to an absolute target when called with nonzero PC, but leaves a relative displacement when PC is zero ([decoder.hpp#L670](../../include/tinyarmsim/decoder.hpp#L670)). Interpreter entry gates on NZCV before opcode dispatch ([interpreter.hpp#L67](../../include/tinyarmsim/interpreter.hpp#L67), [#L108](../../include/tinyarmsim/interpreter.hpp#L108)); branch dispatch resolves relative targets from current PC+4 ([interpreter.hpp#L422](../../include/tinyarmsim/interpreter.hpp#L422)). Decoder and interpreter are coupled through `is_relative` and `imm`.

## Expand–Migrate–Contract matrix
| Phase | Branch slice |
|---|---|
| Expand | Keep T3 decode and conditional execution behind existing `Opcode::B`/condition fields; verify exact assembler words at PC=0 and nonzero PC. |
| Migrate | Migrate acceptance vectors to independently assembled forward/backward taken/untaken cases and assert full NZCV plus PC. |
| Contract | Remove stale hand-encoded assumptions only after all verified vectors pass; no product change indicated by this probe. |

## Probe status
`REPORT_ONLY`: `/usr/bin/arm-none-eabi-as`, `/usr/bin/arm-none-eabi-objdump`, and `/usr/bin/g++ -std=c++20 -Wall -Wextra` probe completed. Product and test sources were not modified. Micro structure: [03-micro.md](03-micro.md).
