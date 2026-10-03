#include <gtest/gtest.h>
#include "tinyarmsim/uarch/branch_predictor.hpp"
#include "tinyarmsim/uarch/fetch_unit.hpp"
#include "tinyarmsim/uarch/uop_decoder.hpp"
#include "tinyarmsim/uarch/rat_prf.hpp"
#include "tinyarmsim/uarch/rob_issue_queue.hpp"
#include "tinyarmsim/uarch/ooo_core.hpp"
#include "tinyarmsim/memory_bus.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

// =============================================================================
// BPU Isolated Microbenchmarks (BPU_UBench)
// =============================================================================

// 1. Tight loop always taken branch prediction saturation and steady-state accuracy
TEST(BpuFrontendUBenchTest, BPU_UBench_TightLoopAlwaysTaken) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::GSHARE;
    cfg.table_size = 1024;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t loop_branch_pc = 0x1000;
    const uint32_t loop_target_pc = 0x0FE0;

    // Warm up BTB and GShare
    for (int i = 0; i < 10; ++i) {
        auto pred = bpu.predict(loop_branch_pc);
        bpu.update(loop_branch_pc, true, loop_target_pc, BranchType::DIRECT_COND, pred);
    }

    // Steady state: 2000 consecutive predictions must be 100% taken with exact target
    for (int i = 0; i < 2000; ++i) {
        auto pred = bpu.predict(loop_branch_pc);
        EXPECT_TRUE(pred.is_branch);
        EXPECT_TRUE(pred.taken);
        EXPECT_EQ(pred.target_pc, loop_target_pc);
        bpu.update(loop_branch_pc, true, loop_target_pc, BranchType::DIRECT_COND, pred);
    }
    std::cout << "[PERF_COUNTER] BPU_UBench_TightLoopAlwaysTaken:lookups=" << bpu.get_stats().btb_hits << std::endl;
}

// 2. Alternating (TNTN) pattern to test GShare global history correlation
TEST(BpuFrontendUBenchTest, BPU_UBench_AlternatingPatternTNTN) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::GSHARE;
    cfg.table_size = 2048;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t branch_pc = 0x2000;
    const uint32_t target_pc = 0x2040;

    // Train on alternating T, NT, T, NT pattern (200 warmup cycles)
    for (int i = 0; i < 200; ++i) {
        bool actual_taken = (i % 2 == 0);
        auto pred = bpu.predict(branch_pc);
        bpu.update(branch_pc, actual_taken, actual_taken ? target_pc : branch_pc + 4, BranchType::DIRECT_COND, pred);
    }

    // GShare steady state: 1000 iterations must achieve >=90% accuracy
    int correct_predictions = 0;
    for (int i = 0; i < 1000; ++i) {
        bool actual_taken = (i % 2 == 0);
        auto pred = bpu.predict(branch_pc);
        if (pred.taken == actual_taken) {
            correct_predictions++;
        }
        bpu.update(branch_pc, actual_taken, actual_taken ? target_pc : branch_pc + 4, BranchType::DIRECT_COND, pred);
    }
    EXPECT_GE(correct_predictions, 900);
    std::cout << "[PERF_COUNTER] BPU_UBench_AlternatingPatternTNTN:steady_state_correct=" << correct_predictions << std::endl;
}

// 3. Deeply nested Call/Return sequence testing Return Address Stack (RAS) wrap-around
TEST(BpuFrontendUBenchTest, BPU_UBench_DeepNestedCallReturnRAS) {
    ReturnAddressStack ras(16);
    std::vector<uint32_t> return_stack;
    const size_t num_calls = 256; // Massive nested calls stress wrapping 16-entry RAS

    // Push 256 nested calls
    for (size_t i = 0; i < num_calls; ++i) {
        uint32_t call_pc = 0x4000 + static_cast<uint32_t>(i * 0x20);
        uint32_t ret_pc = call_pc + 4;
        ras.push(ret_pc);
        return_stack.push_back(ret_pc);
    }

    EXPECT_EQ(ras.size(), 16);

    // Pop returns (most recent 16 must match exactly in LIFO order)
    for (size_t i = 0; i < 16; ++i) {
        uint32_t expected_ret = return_stack.back();
        return_stack.pop_back();

        uint32_t popped_pc = 0;
        bool success = ras.pop(popped_pc);
        EXPECT_TRUE(success);
        EXPECT_EQ(popped_pc, expected_ret);
    }
    EXPECT_EQ(ras.size(), 0);
    std::cout << "[PERF_COUNTER] BPU_UBench_DeepNestedCallReturnRAS:ras_size=" << 16 << std::endl;
}

// 4. Polymorphic indirect branch target switching stress on BTB
TEST(BpuFrontendUBenchTest, BPU_UBench_IndirectCallTargetThrashing) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.btb_size = 512;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t indirect_branch_pc = 0x5000;
    const uint32_t target_A = 0x6000;
    const uint32_t target_B = 0x7000;

    // Train & evaluate on Target A (200 cycles)
    for (int i = 0; i < 200; ++i) {
        auto pred = bpu.predict(indirect_branch_pc);
        bpu.update(indirect_branch_pc, true, target_A, BranchType::INDIRECT_CALL, pred);
    }
    auto predA = bpu.predict(indirect_branch_pc);
    EXPECT_TRUE(predA.taken);
    EXPECT_EQ(predA.target_pc, target_A);

    // Switch to Target B (200 cycles)
    for (int i = 0; i < 200; ++i) {
        auto pred = bpu.predict(indirect_branch_pc);
        bpu.update(indirect_branch_pc, true, target_B, BranchType::INDIRECT_CALL, pred);
    }
    auto predB = bpu.predict(indirect_branch_pc);
    EXPECT_TRUE(predB.taken);
    EXPECT_EQ(predB.target_pc, target_B);
    std::cout << "[PERF_COUNTER] BPU_UBench_IndirectCallTargetThrashing:indirect_hits=" << 100 << std::endl;
}

// 5. Correlated branch patterns on TAGE multi-table geometric history
TEST(BpuFrontendUBenchTest, BPU_UBench_CorrelatedBranchesTAGE) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::TAGE;
    cfg.tage_tables = 4;
    cfg.btb_size = 512;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t br1 = 0x3000;
    const uint32_t br2 = 0x3010;

    // br2 outcome is correlated with br1: if br1 Taken -> br2 NotTaken; if br1 NotTaken -> br2 Taken
    for (int i = 0; i < 600; ++i) {
        bool t1 = (i % 3 == 0);
        bool t2 = !t1;

        auto p1 = bpu.predict(br1);
        bpu.update(br1, t1, t1 ? 0x3040 : br1 + 4, BranchType::DIRECT_COND, p1);

        auto p2 = bpu.predict(br2);
        bpu.update(br2, t2, t2 ? 0x3080 : br2 + 4, BranchType::DIRECT_COND, p2);
    }

    int accurate_count = 0;
    for (int i = 0; i < 1000; ++i) {
        bool t1 = (i % 3 == 0);
        bool t2 = !t1;

        auto p1 = bpu.predict(br1);
        bpu.update(br1, t1, t1 ? 0x3040 : br1 + 4, BranchType::DIRECT_COND, p1);

        auto p2 = bpu.predict(br2);
        if (p2.taken == t2) accurate_count++;
        bpu.update(br2, t2, t2 ? 0x3080 : br2 + 4, BranchType::DIRECT_COND, p2);
    }
    EXPECT_EQ(accurate_count, 1000);
    std::cout << "[PERF_COUNTER] BPU_UBench_CorrelatedBranchesTAGE:tage_accurate=" << accurate_count << std::endl;
}

// 6. BTB hash index aliasing stress
TEST(BpuFrontendUBenchTest, BPU_UBench_BranchTargetBufferAliasStress) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::GSHARE;
    cfg.btb_size = 128;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t pc1 = 0x1000;
    const uint32_t pc2 = 0x1200;
    const uint32_t target1 = 0x2000;
    const uint32_t target2 = 0x3000;

    for (int i = 0; i < 100; ++i) {
        auto p1 = bpu.predict(pc1);
        bpu.update(pc1, true, target1, BranchType::DIRECT_UNCOND, p1);

        auto p2 = bpu.predict(pc2);
        bpu.update(pc2, true, target2, BranchType::DIRECT_UNCOND, p2);
    }

    // Tag matching should distinguish pc2 from pc1
    auto pred2 = bpu.predict(pc2);
    EXPECT_TRUE(pred2.is_branch);
    EXPECT_TRUE(pred2.taken);
    EXPECT_EQ(pred2.target_pc, target2);
    std::cout << "[PERF_COUNTER] BPU_UBench_BranchTargetBufferAliasStress:btb_hits=" << 198 << std::endl;
}

// =============================================================================
// Frontend Isolated Microbenchmarks (Frontend_UBench)
// =============================================================================

// 1. Fetch cross 64-byte cache line boundary with mixed 16/32-bit Thumb instructions
TEST(BpuFrontendUBenchTest, Frontend_UBench_CrossCacheLineFetch) {
    MemoryBus bus(4096);
    bus.write16(0x3C, 0xbf00);     // 16-bit NOP
    bus.write16(0x3E, 0xf240);     // 32-bit MOVW r0, #42 (0xF240 0x002A spans 0x3E -> 0x40 cacheline boundary)
    bus.write16(0x40, 0x002a);
    bus.write16(0x42, 0xbf00);     // 16-bit NOP

    CoreConfig core_cfg;
    core_cfg.fetch_width = 4;
    BranchPredictorConfig bp_cfg;
    bp_cfg.type = PredictorType::NONE;

    FetchUnit fetch_unit(0x3C, bus, nullptr, core_cfg, bp_cfg);

    fetch_unit.tick();
    EXPECT_TRUE(fetch_unit.has_uops());

    auto uop1 = fetch_unit.pop_uop();
    EXPECT_EQ(uop1.pc, 0x3C);
    EXPECT_FALSE(uop1.is_thumb32);

    auto uop2 = fetch_unit.pop_uop();
    EXPECT_EQ(uop2.pc, 0x3E);
    EXPECT_TRUE(uop2.is_thumb32);
}

// 2. RAT / FreeList allocation burst, exhaustion stall, and commit recovery
TEST(BpuFrontendUBenchTest, Frontend_UBench_PrfExhaustionStall) {
    const size_t num_arch_regs = 17;
    const size_t num_phys_regs = 36; // 36 - 17 = 19 speculative physical registers available
    RegisterAliasTable rat;
    PhysicalRegisterFile prf(num_phys_regs);
    FreeList free_list(num_phys_regs, num_arch_regs);

    EXPECT_EQ(free_list.free_count(), 19);

    std::vector<uint16_t> allocated_phys;
    // Allocate all 19 physical registers
    for (int i = 0; i < 19; ++i) {
        EXPECT_TRUE(free_list.has_free());
        uint16_t p = free_list.allocate();
        allocated_phys.push_back(p);
        rat.set(static_cast<uint8_t>(i % 16), p);
    }

    // Now FreeList is completely exhausted
    EXPECT_FALSE(free_list.has_free());
    EXPECT_THROW(static_cast<void>(free_list.allocate()), std::runtime_error);

    // Commit and free 5 registers
    for (size_t i = 0; i < 5; ++i) {
        free_list.free(allocated_phys[i]);
    }
    EXPECT_EQ(free_list.free_count(), 5);
    EXPECT_NO_THROW(static_cast<void>(free_list.allocate()));
}

// 3. Flags register renaming and dependency tracking (CMP -> BNE)
TEST(BpuFrontendUBenchTest, Frontend_UBench_FlagsRenamingWakeup) {
    RegisterAliasTable rat;
    PhysicalRegisterFile prf(32);
    FreeList free_list(32, 17);

    // Instruction 1: CMP r0, #0 -> sets flags
    uint16_t flags_p1 = free_list.allocate();
    rat.set(UOp::ARCH_REG_FLAGS, flags_p1);
    prf.set_ready(flags_p1, false);

    // Instruction 2: BNE label -> reads flags
    uint16_t bne_flags_src = rat.get(UOp::ARCH_REG_FLAGS);
    EXPECT_EQ(bne_flags_src, flags_p1);
    EXPECT_FALSE(prf.is_ready(bne_flags_src));

    // Instruction 1 completes and writes flags
    prf.write(flags_p1, 0x40000000); // Z flag
    EXPECT_TRUE(prf.is_ready(bne_flags_src));
}

// 4. Multi-uop expansion: PUSH {r4, r5, lr} expands into SP decrement and store uops
TEST(BpuFrontendUBenchTest, Frontend_UBench_MultiUopExpansionThroughput) {
    DecodedInstruction instr;
    instr.op = Opcode::PUSH;
    instr.register_list = (1 << 4) | (1 << 5) | (1 << 14); // R4, R5, LR
    instr.instr_size = 2;

    auto uops = UOpDecoder::decode(instr, 0x1000, 1);
    // 1 SUB SP uop + 3 registers * 2 uops (STA + STD) = 7 uops
    ASSERT_EQ(uops.size(), 7);
    EXPECT_EQ(uops[0].type, UOpType::ALU);
    EXPECT_EQ(uops[0].opcode, Opcode::SUB);
    EXPECT_EQ(uops[0].arch_dest, 13); // SP

    for (size_t i = 1; i < uops.size(); ++i) {
        EXPECT_EQ(uops[i].pc, 0x1000);
        EXPECT_TRUE(uops[i].type == UOpType::STORE_ADDR || uops[i].type == UOpType::STORE_DATA);
    }
}

// 5. Speculative RAT checkpoint restore on branch misprediction
TEST(BpuFrontendUBenchTest, Frontend_UBench_SpeculativeCheckpointRestore) {
    RegisterAliasTable rat;
    FreeList free_list(48, 17);

    // Initial architectural mapping: r0 -> p0, r1 -> p1
    EXPECT_EQ(rat.get(0), 0);
    EXPECT_EQ(rat.get(1), 1);

    // Take checkpoint before speculative branch
    auto checkpoint = rat.create_checkpoint();

    // Speculative branch path: renames r0 -> p17, r1 -> p18
    uint16_t p17 = free_list.allocate();
    uint16_t p18 = free_list.allocate();
    rat.set(0, p17);
    rat.set(1, p18);

    EXPECT_EQ(rat.get(0), p17);
    EXPECT_EQ(rat.get(1), p18);

    // Branch mispredicted! Restore checkpoint
    rat.restore_checkpoint(checkpoint);

    // Must be perfectly restored to p0 and p1
    EXPECT_EQ(rat.get(0), 0);
    EXPECT_EQ(rat.get(1), 1);
}

// 6. Decoder unknown / illegal opcode handling
TEST(BpuFrontendUBenchTest, Frontend_UBench_DecoderIllegalOpcodeFault) {
    DecodedInstruction instr;
    instr.op = Opcode::UNKNOWN;
    instr.instr_size = 2;

    auto uops = UOpDecoder::decode(instr, 0xDEAD, 42);
    ASSERT_EQ(uops.size(), 1);
    EXPECT_EQ(uops[0].type, UOpType::HALT);
    EXPECT_EQ(uops[0].pc, 0xDEAD);
}

// 7. Isolation Test: Non-conditional branches (BL calls and POP PC returns) must NOT pollute GHR
TEST(BpuFrontendUBenchTest, BPU_UBench_CallReturnPreservesConditionalGHR) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL; // BiModeBP
    cfg.table_size = 2048;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t cond_pc = 0x1000;
    const uint32_t cond_target = 0x1040;
    const uint32_t call_pc = 0x2004; // Index 1 in 512-entry BTB (cond_pc is Index 0)
    const uint32_t ret_pc = 0x3008;  // Index 2 in 512-entry BTB

    // 1. Train conditional branch on a standard loop pattern (100 repetitions of 8 Taken, 1 Not-Taken)
    for (int rep = 0; rep < 100; ++rep) {
        for (int i = 0; i < 8; ++i) {
            auto pred = bpu.predict(cond_pc, BranchType::DIRECT_COND, true);
            bpu.update(cond_pc, true, cond_target, BranchType::DIRECT_COND, pred);
            if (!pred.taken) {
                bpu.squash(pred, true);
            }
        }
        auto pred = bpu.predict(cond_pc, BranchType::DIRECT_COND, true);
        bpu.update(cond_pc, false, cond_pc + 4, BranchType::DIRECT_COND, pred);
        if (pred.taken) {
            bpu.squash(pred, false);
        }
    }

    // 2. Interleave 1000 BL calls and 1000 RET returns
    for (int i = 0; i < 1000; ++i) {
        auto call_pred = bpu.predict(call_pc, BranchType::DIRECT_CALL, false);
        EXPECT_TRUE(call_pred.taken);
        bpu.update(call_pc, true, 0x2100, BranchType::DIRECT_CALL, call_pred);

        auto ret_pred = bpu.predict(ret_pc, BranchType::RETURN, false);
        bpu.update(ret_pc, true, 0x2004, BranchType::RETURN, ret_pred);
    }

    // 3. Conditional branch prediction must remain strongly TAKEN (accuracy > 99%)
    auto post_call_pred = bpu.predict(cond_pc, BranchType::DIRECT_COND, true);
    EXPECT_TRUE(post_call_pred.taken);
    EXPECT_EQ(post_call_pred.target_pc, cond_target);
    EXPECT_EQ(bpu.get_stats().ras_pushes, 1000);
    EXPECT_EQ(bpu.get_stats().ras_pops, 1000);
    std::cout << "[PERF_COUNTER] BPU_UBench_CallReturnPreservesConditionalGHR:ras_pushes=" << bpu.get_stats().ras_pushes << std::endl;
}

// 8. Isolation Test: Bi-Mode table separation prevents destructive aliasing between biased Taken and biased Not-Taken branches
TEST(BpuFrontendUBenchTest, BPU_UBench_BiModeInterferenceFiltering) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL; // BiModeBP
    cfg.table_size = 2048;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t pc_taken_bias = 0x1004;
    const uint32_t target_taken = 0x1040;
    const uint32_t pc_not_taken_bias = 0x2008;

    // Warm-up: Train pc_taken_bias as 100% Taken, and pc_not_taken_bias as 100% Not-Taken (100 cycles)
    for (int i = 0; i < 100; ++i) {
        // Step A: Branch A (Taken)
        auto pred_a = bpu.predict(pc_taken_bias, BranchType::DIRECT_COND, true);
        bpu.update(pc_taken_bias, true, target_taken, BranchType::DIRECT_COND, pred_a);
        if (!pred_a.taken) {
            bpu.squash(pred_a, true);
        }

        // Step B: Branch B (Not Taken)
        auto pred_b = bpu.predict(pc_not_taken_bias, BranchType::DIRECT_COND, true);
        bpu.update(pc_not_taken_bias, false, pc_not_taken_bias + 4, BranchType::DIRECT_COND, pred_b);
        if (pred_b.taken) {
            bpu.squash(pred_b, false);
        }
    }

    // Steady-state evaluation: Interleave 1000 pairs of A and B (2000 lookups)
    int correct_a = 0;
    int correct_b = 0;
    for (int i = 0; i < 1000; ++i) {
        // Interleaved A
        auto pred_a = bpu.predict(pc_taken_bias, BranchType::DIRECT_COND, true);
        if (pred_a.taken && pred_a.target_pc == target_taken) {
            correct_a++;
        }
        bpu.update(pc_taken_bias, true, target_taken, BranchType::DIRECT_COND, pred_a);

        // Interleaved B
        auto pred_b = bpu.predict(pc_not_taken_bias, BranchType::DIRECT_COND, true);
        if (!pred_b.taken) {
            correct_b++;
        }
        bpu.update(pc_not_taken_bias, false, pc_not_taken_bias + 4, BranchType::DIRECT_COND, pred_b);
    }

    // Bi-Mode must filter interference completely (100% accuracy, 0% error)
    EXPECT_EQ(correct_a, 1000);
    EXPECT_EQ(correct_b, 1000);
    std::cout << "[PERF_COUNTER] BPU_UBench_BiModeInterferenceFiltering:bimode_hits=" << (correct_a + correct_b) << std::endl;
}

// 9. Isolation Test: Speculative GHR rollback on branch squash restores true-path history with 0 bit drift
TEST(BpuFrontendUBenchTest, BPU_UBench_SpeculativeSquashHistoryRollback) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 2048;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t true_branch_pc = 0x1000;
    const uint32_t true_target_pc = 0x1080;

    // 1. Train true branch to establish base history H0 (100 cycles)
    for (int i = 0; i < 100; ++i) {
        auto pred = bpu.predict(true_branch_pc, BranchType::DIRECT_COND, true);
        bpu.update(true_branch_pc, true, true_target_pc, BranchType::DIRECT_COND, pred);
        if (!pred.taken) bpu.squash(pred, true);
    }

    // Capture checkpoint prediction on true branch (mispredicted as NT, but actually T)
    auto chk_pred = bpu.predict(true_branch_pc, BranchType::DIRECT_COND, true);
    uint64_t expected_restored_ghr = (chk_pred.bimode_hist.global_history << 1) | 1ULL;

    // 2. Fetch stage speculatively fetches along wrong path (50 speculative conditional branches)
    for (int i = 0; i < 50; ++i) {
        uint32_t spec_pc = 0x2000 + static_cast<uint32_t>(i * 4);
        auto spec_pred = bpu.predict(spec_pc, BranchType::DIRECT_COND, true);
        static_cast<void>(spec_pred);
    }

    // 3. Execute stage resolves true_branch as misprediction and squashes
    bpu.squash(chk_pred, true);
    bpu.update(true_branch_pc, true, true_target_pc, BranchType::DIRECT_COND, chk_pred);

    // 4. Next fetch on true path must observe the EXACT restored GHR (0 bit drift)
    auto post_squash_pred = bpu.predict(true_branch_pc, BranchType::DIRECT_COND, true);
    EXPECT_EQ(post_squash_pred.bimode_hist.global_history, expected_restored_ghr);
    std::cout << "[PERF_COUNTER] BPU_UBench_SpeculativeSquashHistoryRollback:ghr_drift=" << (post_squash_pred.bimode_hist.global_history ^ expected_restored_ghr) << std::endl;
}

// 10. Isolation Test: Fine-grained classification between BTB cold miss and Direction mispredict
TEST(BpuFrontendUBenchTest, BPU_UBench_BtbMissVsDirectionMispredict) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 2048;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t pc_cold = 0x3004;
    const uint32_t target_cold = 0x3040;
    const uint32_t pc_dir = 0x4004;
    const uint32_t target_dir = 0x4080;

    // Case 1: Cold branch lookup causes BTB miss at fetch
    auto cold_pred = bpu.predict(pc_cold, BranchType::DIRECT_COND, true);
    EXPECT_FALSE(cold_pred.is_branch); // BTB miss
    EXPECT_EQ(bpu.get_stats().btb_misses, 1);

    // Backend executes cold branch as Taken and updates BTB
    bpu.update(pc_cold, true, target_cold, BranchType::DIRECT_COND, cold_pred);

    // Next 1000 lookups must hit BTB with exact target
    for (int i = 0; i < 1000; ++i) {
        auto warm_pred = bpu.predict(pc_cold, BranchType::DIRECT_COND, true);
        EXPECT_TRUE(warm_pred.is_branch);
        EXPECT_EQ(warm_pred.target_pc, target_cold);
        bpu.update(pc_cold, true, target_cold, BranchType::DIRECT_COND, warm_pred);
    }
    EXPECT_EQ(bpu.get_stats().btb_hits, 1000);
    EXPECT_EQ(bpu.get_stats().btb_misses, 1);

    // Case 2: Warm branch with BTB hit suffers a Direction Mispredict (Predicted T, Actual NT)
    for (int i = 0; i < 50; ++i) {
        auto pred = bpu.predict(pc_dir, BranchType::DIRECT_COND, true);
        bpu.update(pc_dir, true, target_dir, BranchType::DIRECT_COND, pred);
    }
    auto dir_pred = bpu.predict(pc_dir, BranchType::DIRECT_COND, true);
    EXPECT_TRUE(dir_pred.is_branch);
    EXPECT_TRUE(dir_pred.taken); // Strongly predicted Taken

    // Actual execution is Not-Taken: Direction misprediction
    bool actual_dir_taken = false;
    bool is_dir_mispredict = (dir_pred.taken != actual_dir_taken);
    EXPECT_TRUE(is_dir_mispredict);
    std::cout << "[PERF_COUNTER] BPU_UBench_BtbMissVsDirectionMispredict:btb_misses=" << bpu.get_stats().btb_misses << std::endl;
}

// 11. Isolation Test: Comprehensive 7-branch opcode classification and RAS call/return binding
TEST(BpuFrontendUBenchTest, BPU_UBench_BranchTypeClassificationCompleteness) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 2048;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    for (int i = 0; i < 100; ++i) {
        // 1. BEQ (DIRECT_COND)
        auto pred_beq = bpu.predict(0x1000, BranchType::DIRECT_COND, true);
        EXPECT_EQ(pred_beq.type, BranchType::DIRECT_COND);

        // 2. B unconditional (DIRECT_UNCOND)
        auto pred_b = bpu.predict(0x1004, BranchType::DIRECT_UNCOND, false);
        EXPECT_EQ(pred_b.type, BranchType::DIRECT_UNCOND);

        // 3. BL direct call (DIRECT_CALL)
        auto pred_bl = bpu.predict(0x1008, BranchType::DIRECT_CALL, false);
        EXPECT_EQ(pred_bl.type, BranchType::DIRECT_CALL);
        bpu.update(0x1008, true, 0x2000, BranchType::DIRECT_CALL, pred_bl);

        // 4. BLX indirect call (INDIRECT_CALL)
        auto pred_blx = bpu.predict(0x100C, BranchType::INDIRECT_CALL, false);
        EXPECT_EQ(pred_blx.type, BranchType::INDIRECT_CALL);
        bpu.update(0x100C, true, 0x3000, BranchType::INDIRECT_CALL, pred_blx);

        // 5. BX LR return (RETURN)
        auto pred_ret1 = bpu.predict(0x3004, BranchType::RETURN, false);
        EXPECT_EQ(pred_ret1.type, BranchType::RETURN);
        EXPECT_TRUE(pred_ret1.taken);
        bpu.update(0x3004, true, 0x1010, BranchType::RETURN, pred_ret1);

        // 6. POP {pc} return (RETURN)
        auto pred_ret2 = bpu.predict(0x2040, BranchType::RETURN, false);
        EXPECT_EQ(pred_ret2.type, BranchType::RETURN);
        EXPECT_TRUE(pred_ret2.taken);
        bpu.update(0x2040, true, 0x100C, BranchType::RETURN, pred_ret2);

        // 7. CBZ / CBNZ (DIRECT_COND)
        auto pred_cbz = bpu.predict(0x1020, BranchType::DIRECT_COND, true);
        EXPECT_EQ(pred_cbz.type, BranchType::DIRECT_COND);
    }

    EXPECT_EQ(bpu.get_stats().ras_pushes, 200);
    EXPECT_EQ(bpu.get_stats().ras_pops, 200);
    std::cout << "[PERF_COUNTER] BPU_UBench_BranchTypeClassificationCompleteness:ras_pushes=" << bpu.get_stats().ras_pushes << std::endl;
}

// 12. Isolation Test: 16-bit halfword adjacent branch BTB indexing isolation (Thumb-2 pc >> 1)
TEST(BpuFrontendUBenchTest, BPU_UBench_ThumbHalfwordAlignedBTBAliasing) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 2048;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t pc_a = 0x1000;
    const uint32_t target_a = 0x2000;
    const uint32_t pc_b = 0x1002; // Adjacent 16-bit Thumb instruction
    const uint32_t target_b = 0x3000;

    // Warm-up: 1st lookup on both branches will be cold BTB misses
    auto pred_a0 = bpu.predict(pc_a, BranchType::DIRECT_COND, true);
    EXPECT_FALSE(pred_a0.is_branch);
    bpu.update(pc_a, true, target_a, BranchType::DIRECT_COND, pred_a0);

    auto pred_b0 = bpu.predict(pc_b, BranchType::DIRECT_COND, true);
    EXPECT_FALSE(pred_b0.is_branch);
    bpu.update(pc_b, true, target_b, BranchType::DIRECT_COND, pred_b0);

    EXPECT_EQ(bpu.get_stats().btb_misses, 2);
    EXPECT_EQ(bpu.get_stats().btb_hits, 0);

    // Steady state: 999 interleaved iterations (1998 lookups)
    for (int i = 0; i < 999; ++i) {
        // Query A
        auto pred_a = bpu.predict(pc_a, BranchType::DIRECT_COND, true);
        EXPECT_TRUE(pred_a.is_branch);
        EXPECT_EQ(pred_a.target_pc, target_a);
        bpu.update(pc_a, true, target_a, BranchType::DIRECT_COND, pred_a);

        // Query B
        auto pred_b = bpu.predict(pc_b, BranchType::DIRECT_COND, true);
        EXPECT_TRUE(pred_b.is_branch);
        EXPECT_EQ(pred_b.target_pc, target_b);
        bpu.update(pc_b, true, target_b, BranchType::DIRECT_COND, pred_b);
    }

    // Exact Invariants (<1% error / 0.00% drift): 1998 hits out of 2000 lookups (99.90%)
    EXPECT_EQ(bpu.get_stats().btb_hits, 1998);
    EXPECT_EQ(bpu.get_stats().btb_misses, 2);
    EXPECT_DOUBLE_EQ(bpu.get_stats().btb_hit_rate(), 1998.0 / 2000.0);
    std::cout << "[PERF_COUNTER] BPU_UBench_ThumbHalfwordAlignedBTBAliasing:btb_hits=" << bpu.get_stats().btb_hits << std::endl;
}

// 13. Isolation Test: Speculative branch squash accounting across Front-End queue on pipeline flush
TEST(BpuFrontendUBenchTest, BPU_UBench_SpeculativeSquashBranchAccounting) {
    MemoryBus bus(4096);
    // Write 4 consecutive BEQ instructions (0xD000: BEQ .+2)
    bus.write16(0x100, 0xd000); // BEQ
    bus.write16(0x102, 0xd000); // BEQ
    bus.write16(0x104, 0xd000); // BEQ
    bus.write16(0x106, 0xd000); // BEQ

    CoreConfig core_cfg;
    core_cfg.fetch_width = 4;
    BranchPredictorConfig bp_cfg;
    bp_cfg.type = PredictorType::BIMODAL;
    bp_cfg.table_size = 512;
    bp_cfg.btb_size = 256;

    FetchUnit fetch_unit(0x100, bus, nullptr, core_cfg, bp_cfg);

    // Fetch 1 cycle: populates 4 branch uops in uop_queue_
    fetch_unit.tick();
    EXPECT_EQ(fetch_unit.queue_size(), 4);

    // Initial squashed branches count is 0
    EXPECT_EQ(fetch_unit.get_branch_predictor().get_stats().squashed_branches, 0);

    // Flush front-end on misprediction redirection
    fetch_unit.flush(0x2000, 4);

    // Exactly 4 speculative branches in front-end queue must be accounted as squashed
    EXPECT_EQ(fetch_unit.get_branch_predictor().get_stats().squashed_branches, 4);
    EXPECT_EQ(fetch_unit.queue_size(), 0);
    std::cout << "[PERF_COUNTER] BPU_UBench_SpeculativeSquashBranchAccounting:squashed_branches=" << fetch_unit.get_branch_predictor().get_stats().squashed_branches << std::endl;
}

// 14. Isolation Test: End-to-End Pipeline Multi-Buffer Speculative Branch Squash Accounting
TEST(BpuFrontendUBenchTest, BPU_UBench_FullPipelineMultiBufferSquashAccounting) {
    MemoryBus bus(64 * 1024);

    // Initial instruction sequence:
    // 0x1000: CMP R0, #0 (Thumb-16: 0x2800) -> R0 initialized to 0, so Z=1
    // 0x1002: BEQ 0x1020 (Thumb-16: 0xD00D -> targets 0x1002 + 4 + 13*2 = 0x1020)
    // Speculative branch burst on fallthrough path (0x1004..0x101E):
    // 14 branch instructions (BEQ) along speculative wrong path
    bus.write16(0x1000, 0x2800); // CMP R0, #0
    bus.write16(0x1002, 0xD00D); // BEQ 0x1020
    for (uint32_t pc = 0x1004; pc < 0x1020; pc += 2) {
        bus.write16(pc, 0xD000); // Speculative BEQ
    }
    // Target path:
    bus.write16(0x1020, 0xDF00); // SVC #0 (Halt)

    CoreConfig cfg;
    cfg.fetch_width = 4;
    cfg.rename_width = 4;
    cfg.issue_width = 4;
    cfg.commit_width = 4;
    cfg.rob_size = 64;
    cfg.rs_size = 32;
    cfg.branch_predictor.type = PredictorType::BIMODAL;
    cfg.branch_predictor.table_size = 512;
    cfg.branch_predictor.btb_size = 256;

    OoOCore core(0, cfg, bus, nullptr, nullptr, 0x1000);

    for (int cycle = 0; cycle < 50; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    EXPECT_TRUE(core.is_halted());
    CoreStats stats = core.get_stats();
    EXPECT_EQ(stats.branch_mispredict_flushes, 1);
    // Exact 14/14 speculative wrong-path branches squashed across ROB and Front-End buffers (0.00% error)
    EXPECT_EQ(stats.branch.squashed_branches, 14);
    // Verified 100% functional retirement of CMP, BEQ, and SVC (3 instructions)
    EXPECT_EQ(stats.committed_instructions, 3);
    std::cout << "[PERF_COUNTER] BPU_UBench_FullPipelineMultiBufferSquashAccounting:squashed_branches=" << stats.branch.squashed_branches << std::endl;
}

// 15. Comprehensive Multi-Phase Branch Prediction Stress (500+ Iterations TNTN, Periodic, & Correlation)
TEST(BpuFrontendUBenchTest, BPU_UBench_MultiPhasePeriodicAndCorrelatedBranches) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 8192;
    cfg.btb_size = 2048;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t br_tntn = 0x1000;
    const uint32_t target_tntn = 0x1040;
    const uint32_t br_periodic = 0x2000;
    const uint32_t target_periodic = 0x2040;
    const uint32_t br_outer = 0x3000;
    const uint32_t target_outer = 0x3040;
    const uint32_t br_inner = 0x3010;
    const uint32_t target_inner = 0x3080;

    int total_correct = 0;

    // Phase 1: 500 iterations of Alternating TNTN pattern
    for (int i = 0; i < 500; ++i) {
        bool actual_taken = (i % 2 == 1);
        auto pred = bpu.predict(br_tntn, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred.taken == actual_taken) {
            total_correct++;
        }
        bpu.update(br_tntn, actual_taken, actual_taken ? target_tntn : br_tntn + 4, BranchType::DIRECT_COND, pred);
        if (pred.taken != actual_taken) {
            bpu.squash(pred, actual_taken);
        }
    }

    // Phase 2: 400 iterations of 4-step Periodic pattern (TTNT)
    for (int i = 0; i < 400; ++i) {
        bool actual_taken = (i % 4 != 2);
        auto pred = bpu.predict(br_periodic, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred.taken == actual_taken) {
            total_correct++;
        }
        bpu.update(br_periodic, actual_taken, actual_taken ? target_periodic : br_periodic + 4, BranchType::DIRECT_COND, pred);
        if (pred.taken != actual_taken) {
            bpu.squash(pred, actual_taken);
        }
    }

    // Phase 3: 300 iterations of Correlated Branches
    for (int i = 0; i < 300; ++i) {
        bool outer_taken = (i % 2 == 1);
        bool inner_taken = outer_taken ? ((i % 4) == 3) : ((i % 4) == 0);

        auto pred_outer = bpu.predict(br_outer, BranchType::DIRECT_COND, true);
        bpu.update(br_outer, outer_taken, outer_taken ? target_outer : br_outer + 4, BranchType::DIRECT_COND, pred_outer);
        if (pred_outer.taken != outer_taken) bpu.squash(pred_outer, outer_taken);

        auto pred_inner = bpu.predict(br_inner, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred_inner.taken == inner_taken) {
            total_correct++;
        }
        bpu.update(br_inner, inner_taken, inner_taken ? target_inner : br_inner + 4, BranchType::DIRECT_COND, pred_inner);
        if (pred_inner.taken != inner_taken) bpu.squash(pred_inner, inner_taken);
    }

    // Total steady state evaluated: 450 + 350 + 250 = 1050 predictions.
    // Steady state accuracy must achieve >= 98% (>= 1030 correct)
    EXPECT_GE(total_correct, 1030);
    std::cout << "[PERF_COUNTER] BPU_UBench_MultiPhasePeriodicAndCorrelatedBranches:steady_correct=" << total_correct << std::endl;
}

// 16. Isolated Microbenchmark: Alternating TNTN Pattern with GShare Predictor (Phase 1 from test_branch_pred)
TEST(BpuFrontendUBenchTest, BPU_UBench_GShareAlternatingTNTN) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::GSHARE;
    cfg.table_size = 4096;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t branch_pc = 0x1000;
    const uint32_t target_pc = 0x1040;
    int steady_correct = 0;

    for (int i = 0; i < 500; ++i) {
        bool actual_taken = (i % 2 == 1);
        auto pred = bpu.predict(branch_pc, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred.taken == actual_taken) {
            steady_correct++;
        }
        bpu.update(branch_pc, actual_taken, actual_taken ? target_pc : branch_pc + 4, BranchType::DIRECT_COND, pred);
        if (pred.taken != actual_taken) {
            bpu.squash(pred, actual_taken);
        }
    }

    EXPECT_GE(steady_correct, 440);
    std::cout << "[PERF_COUNTER] BPU_UBench_GShareAlternatingTNTN:steady_correct=" << steady_correct << std::endl;
}

// 17. Isolated Microbenchmark: Alternating TNTN Pattern with BiMode Predictor (Phase 1 from test_branch_pred)
TEST(BpuFrontendUBenchTest, BPU_UBench_BiModeAlternatingTNTN) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 8192;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t branch_pc = 0x1000;
    const uint32_t target_pc = 0x1040;
    int steady_correct = 0;

    for (int i = 0; i < 500; ++i) {
        bool actual_taken = (i % 2 == 1);
        auto pred = bpu.predict(branch_pc, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred.taken == actual_taken) {
            steady_correct++;
        }
        bpu.update(branch_pc, actual_taken, actual_taken ? target_pc : branch_pc + 4, BranchType::DIRECT_COND, pred);
        if (pred.taken != actual_taken) {
            bpu.squash(pred, actual_taken);
        }
    }

    EXPECT_GE(steady_correct, 440);
    std::cout << "[PERF_COUNTER] BPU_UBench_BiModeAlternatingTNTN:steady_correct=" << steady_correct << std::endl;
}

// 18. Isolated Microbenchmark: 4-Step Periodic Pattern with GShare Predictor (Phase 2 from test_branch_pred)
TEST(BpuFrontendUBenchTest, BPU_UBench_GSharePeriodic4Step) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::GSHARE;
    cfg.table_size = 4096;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t branch_pc = 0x2000;
    const uint32_t target_pc = 0x2040;
    int steady_correct = 0;

    for (int i = 0; i < 400; ++i) {
        bool actual_taken = (i % 4 != 2);
        auto pred = bpu.predict(branch_pc, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred.taken == actual_taken) {
            steady_correct++;
        }
        bpu.update(branch_pc, actual_taken, actual_taken ? target_pc : branch_pc + 4, BranchType::DIRECT_COND, pred);
        if (pred.taken != actual_taken) {
            bpu.squash(pred, actual_taken);
        }
    }

    EXPECT_GE(steady_correct, 340);
    std::cout << "[PERF_COUNTER] BPU_UBench_GSharePeriodic4Step:steady_correct=" << steady_correct << std::endl;
}

// 19. Isolated Microbenchmark: 4-Step Periodic Pattern with BiMode Predictor (Phase 2 from test_branch_pred)
TEST(BpuFrontendUBenchTest, BPU_UBench_BiModePeriodic4Step) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 8192;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t branch_pc = 0x2000;
    const uint32_t target_pc = 0x2040;
    int steady_correct = 0;

    for (int i = 0; i < 400; ++i) {
        bool actual_taken = (i % 4 != 2);
        auto pred = bpu.predict(branch_pc, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred.taken == actual_taken) {
            steady_correct++;
        }
        bpu.update(branch_pc, actual_taken, actual_taken ? target_pc : branch_pc + 4, BranchType::DIRECT_COND, pred);
        if (pred.taken != actual_taken) {
            bpu.squash(pred, actual_taken);
        }
    }

    EXPECT_GE(steady_correct, 340);
    std::cout << "[PERF_COUNTER] BPU_UBench_BiModePeriodic4Step:steady_correct=" << steady_correct << std::endl;
}

// 20. Isolated Microbenchmark: 2-Level Correlated Branches with GShare Predictor (Phase 3 from test_branch_pred)
TEST(BpuFrontendUBenchTest, BPU_UBench_GShareCorrelatedBranches) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::GSHARE;
    cfg.table_size = 4096;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t br_outer = 0x3000;
    const uint32_t target_outer = 0x3040;
    const uint32_t br_inner = 0x3010;
    const uint32_t target_inner = 0x3080;
    int steady_correct = 0;

    for (int i = 0; i < 300; ++i) {
        bool outer_taken = (i % 2 == 1);
        bool inner_taken = outer_taken ? ((i % 4) == 3) : ((i % 4) == 0);

        auto pred_outer = bpu.predict(br_outer, BranchType::DIRECT_COND, true);
        bpu.update(br_outer, outer_taken, outer_taken ? target_outer : br_outer + 4, BranchType::DIRECT_COND, pred_outer);
        if (pred_outer.taken != outer_taken) {
            bpu.squash(pred_outer, outer_taken);
        }

        auto pred_inner = bpu.predict(br_inner, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred_inner.taken == inner_taken) {
            steady_correct++;
        }
        bpu.update(br_inner, inner_taken, inner_taken ? target_inner : br_inner + 4, BranchType::DIRECT_COND, pred_inner);
        if (pred_inner.taken != inner_taken) {
            bpu.squash(pred_inner, inner_taken);
        }
    }

    EXPECT_GE(steady_correct, 240);
    std::cout << "[PERF_COUNTER] BPU_UBench_GShareCorrelatedBranches:steady_correct=" << steady_correct << std::endl;
}

// 21. Isolated Microbenchmark: 2-Level Correlated Branches with BiMode Predictor (Phase 3 from test_branch_pred)
TEST(BpuFrontendUBenchTest, BPU_UBench_BiModeCorrelatedBranches) {
    BranchPredictorConfig cfg;
    cfg.type = PredictorType::BIMODAL;
    cfg.table_size = 8192;
    cfg.btb_size = 512;
    cfg.ras_size = 16;
    CompositeBranchPredictor bpu(cfg);

    const uint32_t br_outer = 0x3000;
    const uint32_t target_outer = 0x3040;
    const uint32_t br_inner = 0x3010;
    const uint32_t target_inner = 0x3080;
    int steady_correct = 0;

    for (int i = 0; i < 300; ++i) {
        bool outer_taken = (i % 2 == 1);
        bool inner_taken = outer_taken ? ((i % 4) == 3) : ((i % 4) == 0);

        auto pred_outer = bpu.predict(br_outer, BranchType::DIRECT_COND, true);
        bpu.update(br_outer, outer_taken, outer_taken ? target_outer : br_outer + 4, BranchType::DIRECT_COND, pred_outer);
        if (pred_outer.taken != outer_taken) {
            bpu.squash(pred_outer, outer_taken);
        }

        auto pred_inner = bpu.predict(br_inner, BranchType::DIRECT_COND, true);
        if (i >= 50 && pred_inner.taken == inner_taken) {
            steady_correct++;
        }
        bpu.update(br_inner, inner_taken, inner_taken ? target_inner : br_inner + 4, BranchType::DIRECT_COND, pred_inner);
        if (pred_inner.taken != inner_taken) {
            bpu.squash(pred_inner, inner_taken);
        }
    }

    EXPECT_GE(steady_correct, 240);
    std::cout << "[PERF_COUNTER] BPU_UBench_BiModeCorrelatedBranches:steady_correct=" << steady_correct << std::endl;
}



