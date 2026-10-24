<!-- REPORT_META: Task=TASK-050 | Commit=867fdf05b998581223968ba5b690a4202eccada5 -->
# Accepted bare-metal instruction closeout

Status: complete; source integrated by fast-forward after aggregate acceptance on2026-10-24. The original TASK034/035/036 instruction-only/boot scope is closed. Historical research has been archived; its earlier pending-calibration/coverage claims are not current evidence.

## Implementation and acceptance
- Correct STMDB non-SP direction/writeback and wide LDRSB/LDRSH. Narrow multiple-transfer decode so unsupported LDREX/LDRD do not silently execute LDM.
- Explicit firmware ELF Flash-LMA mode retains default VMA-loading semantics. Distinct-LMA/VMA tests contrast both modes.
- Real compiled C firmware executes vector reset and startup.data/BSS initialization. ELF symbols drive poisoning and13 independently expected computed instruction records.
- Preserve original PRIMASK pendingIRQ, CPSIE delivery, EXC_RETURN and high-register/SP restoration tests alongside compiled firmware.
- Fresh default CMake auto-discovers the installed standard Arm compiler; no download/environment override is needed.

## Verification
Fresh configure, fresh firmware build/run, complete build and273/273 CTest: PASS. Integrated workspace build and273/273 tests also pass after restoring original generated ELF assets. Existing isolated ubench gate: ALL PASS(<1%). Independent Standards review has no actionable findings; Spec review has no blockers. Logs: ../../.scratch/baremetal-instruction-closeout/final-verification/. Review: ../../.scratch/baremetal-instruction-closeout/final-review.md.

## Seams and scope
Functional topology: Decoder → OpcodeCache → IsaInterpreter → MemoryBus/NVIC; Loader supplies image bytes and reset state. Coupling is bounded through existing decoded addressing fields and ELF-symbol-based fixture observations. Caller seams are interpreter decode/execute, Loader::load_elf/boot_cortex_m and SoCPeripherals::attach_all. No Perf Mode feature expansion.

Strategy: vertical behavior fixes; expand firmware loading with backward-compatible default, migrate acceptance to compiled outputs, retain prior regression coverage. Target source files: include/tinyarmsim/{decoder.hpp,interpreter.hpp,loader.hpp}, CMakeLists.txt, firmware/apps/isa_boot_acceptance.{c,s} and corresponding tests.

Calibration is complete per user, reference ../gem5; it is not pending. Separate out-of-original-scope prototype observations are documented in ../../.scratch/baremetal-instruction-closeout/followups.md without new queued work. This report does not claim complete ARMv7-M support.
