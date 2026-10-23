# TASK-045 branch evidence

## Independent assembler output
Source assembled with `/usr/bin/arm-none-eabi-as -mcpu=cortex-m3 -mthumb`:

```asm
cases:
  beq.w forward
  bne.w forward
backward:
  beq.w backward
  bne.w backward
forward:
  nop
```

`/usr/bin/arm-none-eabi-objdump -d /tmp/task045.o`:

```text
00000000 <cases>:
   0: f000 8006  beq.w 10 <forward>
   4: f040 8004  bne.w 10 <forward>
00000008 <backward>:
   8: f43f affe  beq.w 8 <backward>
   c: f47f affc  bne.w 8 <backward>
00000010 <forward>:
  10: bf00       nop
```

Halfwords are in assembler display order (first halfword then second). Raw pairs: `F000 8006`, `F040 8004`, `F43F AFFE`, `F47F AFFC`.

## Current-header execution probe
Temporary `/tmp/task045.cpp` included `tinyarmsim/interpreter.hpp`, wrote each assembler word at the stated PC, set flags, decoded with `Decoder::decode32`, then ran one `IsaInterpreter::step()`. Compile command: `/usr/bin/g++ -std=c++20 -Wall -Wextra -Iinclude /tmp/task045.cpp -o /tmp/task045`. It completed without warnings. N,Z,C,V are printed as four bits. Expected PC was the assembled target when taken; otherwise sequential PC+4.

```text
case                 PC   raw      cond imm  rel NZCV expected observed
BEQ forward taken    0000 F000 8006 EQ   000C yes 0110 0010     0010
BEQ forward untaken  0000 F000 8006 EQ   000C yes 0010 0004     0004
BNE forward taken    0004 F040 8004 NE   0010 no  0010 0010     0010
BNE forward untaken  0004 F040 8004 NE   0010 no  0110 0008     0008
BEQ backward taken   0008 F43F AFFE EQ   0008 no  0110 0008     0008
BEQ backward untaken 0008 F43F AFFE EQ   0008 no  0010 000C     000C
BNE backward taken   000C F47F AFFC NE   0008 no  0010 0008     0008
BNE backward untaken 000C F47F AFFC NE   0008 no  0110 0010     0010
```

The command output’s flags are exact: BEQ taken cases used `NZCV=0110` (Z=1); BEQ untaken used `0010` (Z=0); BNE taken used `0010`; BNE untaken used `0110`. C remained 1 and N,V remained 0. All eight observed PCs equal expected PCs. Negative offsets were independently encoded by assembler, not guessed.

## Interpretation / limitation
The probe separates decode, condition gate, and PC update for four valid T3 words. It establishes these paths as healthy for tested BEQ/BNE encodings, so the original failing acceptance is consistent with mistaken raw words, branch base, or expected PC. The quarantined TASK042A diff was not present as a separate commit/change in this checkout; the available `tests/baremetal_firmware_test.cpp` history is the TASK-043 replacement of that earlier in-memory test. Without the exact quarantined assertion text and observed failure log, its individual word/expectation cannot be conclusively classified. No defect was found in the current probe; this is not evidence about every conditional mnemonic or displacement boundary.
