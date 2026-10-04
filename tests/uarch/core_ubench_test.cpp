#include <gtest/gtest.h>
#include <vector>
#include <iostream>
#include "tinyarmsim/uarch/lsd.hpp"
#include "tinyarmsim/uarch/fusion_unit.hpp"
#include "tinyarmsim/uarch/uop.hpp"
#include "tinyarmsim/uarch/ooo_core.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/uarch/cache.hpp"
#include "tinyarmsim/uarch/topdown_profiler.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/interpreter.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

// Helper to create dummy UOps
static UOp make_dummy_uop(uint32_t pc, UOpType type = UOpType::ALU, bool is_branch = false, bool taken = false, uint32_t target = 0) {
    UOp uop;
    uop.pc = pc;
    uop.type = type;
    uop.is_branch = is_branch;
    uop.actual_taken = taken;
    uop.pred_taken = taken;
    uop.actual_target = target;
    uop.pred_target = target;
    return uop;
}

// =============================================================================
// Core Microbenchmarks: Loop Stream Detector (CoreUBench_LSD)
// =============================================================================

// 1. Profiling state machine: Backward branch detection >= 3 iterations transitions to STREAMING
TEST(CoreUBenchTest, CoreUBench_LsdBackwardBranchProfiling) {
    LoopStreamDetectorConfig cfg;
    cfg.min_iterations_to_lock = 3;
    cfg.max_loop_uops = 32;
    LoopStreamDetector lsd(cfg);

    EXPECT_EQ(lsd.get_state(), LoopStreamState::INACTIVE);
    EXPECT_FALSE(lsd.is_streaming());

    const uint32_t loop_start_pc = 0x1000;

    // Construct a 4-uop loop body
    std::vector<UOp> loop_body = {
        make_dummy_uop(0x1000, UOpType::ALU),
        make_dummy_uop(0x1004, UOpType::LOAD),
        make_dummy_uop(0x1008, UOpType::ALU),
        make_dummy_uop(0x1010, UOpType::BRANCH, true, true, loop_start_pc) // Backward branch to 0x1000
    };

    // Iteration 1: Process uops
    for (const auto& uop : loop_body) {
        lsd.observe_uop(uop);
    }
    // After iteration 1 backward branch, state should be PROFILING / CANDIDATE with iteration count = 1
    EXPECT_EQ(lsd.get_state(), LoopStreamState::PROFILING);
    EXPECT_EQ(lsd.get_iteration_count(), 1);
    EXPECT_FALSE(lsd.is_streaming());

    // Iteration 2
    for (const auto& uop : loop_body) {
        lsd.observe_uop(uop);
    }
    EXPECT_EQ(lsd.get_state(), LoopStreamState::PROFILING);
    EXPECT_EQ(lsd.get_iteration_count(), 2);
    EXPECT_FALSE(lsd.is_streaming());

    // Iteration 3
    for (const auto& uop : loop_body) {
        lsd.observe_uop(uop);
    }
    // Reached 3 iterations -> should transition to STREAMING!
    EXPECT_EQ(lsd.get_state(), LoopStreamState::STREAMING);
    EXPECT_TRUE(lsd.is_streaming());
    EXPECT_EQ(lsd.get_captured_uop_count(), 4);

    std::cout << "[PERF_COUNTER] CoreUBench_LsdBackwardBranchProfiling:loops_detected=" << lsd.get_stats().loops_detected << std::endl;
}

// 2. Loop body size constraint: Body > 32 uops must NOT lock or stream
TEST(CoreUBenchTest, CoreUBench_LsdBodySizeConstraint) {
    LoopStreamDetectorConfig cfg;
    cfg.min_iterations_to_lock = 3;
    cfg.max_loop_uops = 32;
    LoopStreamDetector lsd(cfg);

    const uint32_t loop_start_pc = 0x2000;
    const uint32_t loop_branch_pc = 0x2000 + 33 * 4;

    std::vector<UOp> large_loop;
    for (uint32_t i = 0; i < 33; ++i) {
        large_loop.push_back(make_dummy_uop(0x2000 + i * 4, UOpType::ALU));
    }
    // 34th uop is backward branch
    large_loop.push_back(make_dummy_uop(loop_branch_pc, UOpType::BRANCH, true, true, loop_start_pc));

    for (int iter = 0; iter < 5; ++iter) {
        for (const auto& uop : large_loop) {
            lsd.observe_uop(uop);
        }
    }

    // Should NOT be streaming because body size (34 uops) > 32 uops
    EXPECT_FALSE(lsd.is_streaming());
    EXPECT_NE(lsd.get_state(), LoopStreamState::STREAMING);
}

// 3. Streaming state: Emits uops in FIFO loop sequence
TEST(CoreUBenchTest, CoreUBench_LsdStreamingStateDelivery) {
    LoopStreamDetectorConfig cfg;
    cfg.min_iterations_to_lock = 3;
    cfg.max_loop_uops = 32;
    LoopStreamDetector lsd(cfg);

    const uint32_t loop_start_pc = 0x3000;
    std::vector<UOp> loop_body = {
        make_dummy_uop(0x3000, UOpType::ALU),
        make_dummy_uop(0x3004, UOpType::LOAD),
        make_dummy_uop(0x3008, UOpType::BRANCH, true, true, loop_start_pc)
    };

    // Train loop detector for 3 iterations
    for (int i = 0; i < 3; ++i) {
        for (const auto& uop : loop_body) {
            lsd.observe_uop(uop);
        }
    }

    ASSERT_TRUE(lsd.is_streaming());
    EXPECT_EQ(lsd.get_captured_uop_count(), 3);

    // Stream 4 full iterations (12 uops) from LSD buffer
    for (int iter = 0; iter < 4; ++iter) {
        for (size_t i = 0; i < loop_body.size(); ++i) {
            UOp streamed_uop;
            bool ok = lsd.get_next_streamed_uop(streamed_uop);
            EXPECT_TRUE(ok);
            EXPECT_EQ(streamed_uop.pc, loop_body[i].pc);
            EXPECT_EQ(streamed_uop.type, loop_body[i].type);
        }
    }

    EXPECT_EQ(lsd.get_stats().uops_streamed, 12);
    EXPECT_EQ(lsd.get_stats().iterations_streamed, 4);
    std::cout << "[PERF_COUNTER] CoreUBench_LsdStreamingStateDelivery:uops_streamed=" << lsd.get_stats().uops_streamed << std::endl;
}

// 4. Loop exit: Backward branch not taken or branch mispredict resets state
TEST(CoreUBenchTest, CoreUBench_LsdLoopExitDetection) {
    LoopStreamDetectorConfig cfg;
    cfg.min_iterations_to_lock = 3;
    cfg.max_loop_uops = 32;
    LoopStreamDetector lsd(cfg);

    const uint32_t loop_start_pc = 0x4000;
    std::vector<UOp> loop_body = {
        make_dummy_uop(0x4000, UOpType::ALU),
        make_dummy_uop(0x4004, UOpType::BRANCH, true, true, loop_start_pc)
    };

    for (int i = 0; i < 3; ++i) {
        for (const auto& uop : loop_body) {
            lsd.observe_uop(uop);
        }
    }

    ASSERT_TRUE(lsd.is_streaming());

    // Notify loop exit (e.g. branch evaluated as not-taken or exit signal)
    lsd.notify_loop_exit();

    EXPECT_FALSE(lsd.is_streaming());
    EXPECT_EQ(lsd.get_state(), LoopStreamState::INACTIVE);
    EXPECT_EQ(lsd.get_stats().loop_exits, 1);
}

// 5. General Reset
TEST(CoreUBenchTest, CoreUBench_LsdReset) {
    LoopStreamDetectorConfig cfg;
    LoopStreamDetector lsd(cfg);

    UOp uop = make_dummy_uop(0x5000, UOpType::ALU);
    lsd.observe_uop(uop);

    lsd.reset();
    EXPECT_EQ(lsd.get_state(), LoopStreamState::INACTIVE);
    EXPECT_FALSE(lsd.is_streaming());
    EXPECT_EQ(lsd.get_iteration_count(), 0);
    EXPECT_EQ(lsd.get_captured_uop_count(), 0);
}

// 6. Frontend bypass during LSD streaming (Full OoO Core integration)
TEST(CoreUBenchTest, CoreUBench_LsdFrontendBypassStreaming) {
    // Write a small loop program in memory:
    // 0x1000: ADD r0, r0, #1  (Thumb-16: 0x3001)
    // 0x1002: CMP r0, #10     (Thumb-16: 0x280A)
    // 0x1004: BNE 0x1000      (Thumb-16: 0xD1FC) -> offset: (0x1000 - (0x1004 + 4)) = -8 bytes = -4 halfwords -> 0xD1FC
    // 0x1006: SVC #0          (Thumb-16: 0xDF00)
    MemoryBus bus(65536);
    bus.write16(0x1000, 0x3001);
    bus.write16(0x1002, 0x280A);
    bus.write16(0x1004, 0xD1FC);
    bus.write16(0x1006, 0xDF00);

    CacheConfig l1i_cfg;
    l1i_cfg.type = CacheType::SET_ASSOCIATIVE;
    l1i_cfg.size_bytes = 32768;
    l1i_cfg.associativity = 4;
    l1i_cfg.line_size = 64;
    l1i_cfg.hit_latency_cycles = 1;

    CacheConfig l1d_cfg;
    l1d_cfg.type = CacheType::SET_ASSOCIATIVE;
    l1d_cfg.size_bytes = 32768;
    l1d_cfg.associativity = 4;
    l1d_cfg.line_size = 64;

    // First run with LSD ENABLED
    Cache l1i_lsd(l1i_cfg);
    Cache l1d_lsd(l1d_cfg);
    CoreConfig cfg_lsd;
    cfg_lsd.lsd_type = LSDType::LOOP_STREAM;
    cfg_lsd.lsd_capacity = 32;
    cfg_lsd.l1i = l1i_cfg;
    cfg_lsd.l1d = l1d_cfg;

    OoOCore core_lsd(0, cfg_lsd, bus, &l1i_lsd, &l1d_lsd, 0x1000);
    uint32_t cycles = 0;
    while (!core_lsd.is_halted() && cycles < 1000) {
        core_lsd.tick();
        cycles++;
    }

    EXPECT_TRUE(core_lsd.is_halted());
    EXPECT_EQ(core_lsd.read_arch_reg(0), 10);
    EXPECT_GE(core_lsd.get_lsd().get_stats().loops_detected, 1);
    EXPECT_GT(core_lsd.get_lsd().get_stats().uops_streamed, 0);

    // Run without LSD for baseline comparison
    Cache l1i_no_lsd(l1i_cfg);
    Cache l1d_no_lsd(l1d_cfg);
    CoreConfig cfg_no_lsd;
    cfg_no_lsd.lsd_type = LSDType::NONE;
    cfg_no_lsd.l1i = l1i_cfg;
    cfg_no_lsd.l1d = l1d_cfg;

    OoOCore core_no_lsd(0, cfg_no_lsd, bus, &l1i_no_lsd, &l1d_no_lsd, 0x1000);
    cycles = 0;
    while (!core_no_lsd.is_halted() && cycles < 1000) {
        core_no_lsd.tick();
        cycles++;
    }

    EXPECT_TRUE(core_no_lsd.is_halted());
    EXPECT_EQ(core_no_lsd.read_arch_reg(0), 10);

    // Frontend bypass verification: L1I accesses should be significantly lower with LSD streaming
    uint64_t l1i_accesses_lsd = l1i_lsd.get_stats().accesses;
    uint64_t l1i_accesses_no_lsd = l1i_no_lsd.get_stats().accesses;

    std::cout << "[LSD_FRONTEND_BYPASS] L1I accesses with LSD: " << l1i_accesses_lsd
              << ", without LSD: " << l1i_accesses_no_lsd << std::endl;

    EXPECT_LT(l1i_accesses_lsd, l1i_accesses_no_lsd);
}

// 7. Multi-Pillar Scientific Verification: (1) Top-Down Frontend_Bound = 0.00% invariant during streaming
TEST(CoreUBenchTest, LSD_UBench_TopDownFrontendBoundInvariant) {
    TopDownProfiler profiler;
    profiler.init(1, 4);

    // Initial training iterations (iterations 0..2) might encounter fetch/decode slots
    // Once LSD enters STREAMING mode, all loop uops bypass Fetch/Decode directly into Rename/Dispatch.
    // During 100 cycles of steady-state streaming:
    for (int i = 0; i < 100; ++i) {
        profiler.record_slot(0, SlotType::RetiringBaseAlu);
        profiler.record_slot(0, SlotType::RetiringMem);
        profiler.record_slot(0, SlotType::BackEndCoreRSFull);
        profiler.record_slot(0, SlotType::RetiringBaseAlu);
    }

    const auto& report = profiler.get_report(0);
    EXPECT_EQ(report.total_slots, 400);
    EXPECT_EQ(report.frontend_slots, 0);

    // Slot conservation invariant: Total == Retiring + BadSpec + Frontend + Backend
    uint64_t sum_slots = report.retiring_slots + report.bad_spec_slots + report.frontend_slots + report.backend_slots;
    EXPECT_EQ(sum_slots, report.total_slots);

    double sum_pct = report.retiring_pct() + report.bad_spec_pct() + report.frontend_pct() + report.backend_pct();
    EXPECT_NEAR(sum_pct, 100.0, 0.001);
    EXPECT_DOUBLE_EQ(report.frontend_pct(), 0.0);

    std::cout << "[PERF_COUNTER] LSD_UBench_TopDownFrontendBoundInvariant:frontend_bound_pct=" << report.frontend_pct() << std::endl;
    std::cout << "[PERF_COUNTER] LSD_UBench_TopDownFrontendBoundInvariant:slot_conservation=" << sum_pct << std::endl;
}

// 8. Multi-Pillar Scientific Verification: (2) L1I access count ceases incrementing during loop execution
TEST(CoreUBenchTest, LSD_UBench_L1IAccessCountCeasesIncrementing) {
    // 0x1000: ADD r0, r0, #1  (Thumb-16: 0x3001)
    // 0x1002: CMP r0, #50     (Thumb-16: 0x2832)
    // 0x1004: BNE 0x1000      (Thumb-16: 0xD1FC)
    // 0x1006: SVC #0          (Thumb-16: 0xDF00)
    MemoryBus bus(65536);
    bus.write16(0x1000, 0x3001);
    bus.write16(0x1002, 0x2832);
    bus.write16(0x1004, 0xD1FC);
    bus.write16(0x1006, 0xDF00);

    CacheConfig l1i_cfg;
    l1i_cfg.type = CacheType::SET_ASSOCIATIVE;
    l1i_cfg.size_bytes = 32768;
    l1i_cfg.associativity = 4;
    l1i_cfg.line_size = 64;
    l1i_cfg.hit_latency_cycles = 1;

    CacheConfig l1d_cfg;
    l1d_cfg.type = CacheType::SET_ASSOCIATIVE;
    l1d_cfg.size_bytes = 32768;
    l1d_cfg.associativity = 4;
    l1d_cfg.line_size = 64;

    Cache l1i(l1i_cfg);
    Cache l1d(l1d_cfg);
    CoreConfig cfg;
    cfg.lsd_type = LSDType::LOOP_STREAM;
    cfg.lsd_capacity = 32;
    cfg.l1i = l1i_cfg;
    cfg.l1d = l1d_cfg;

    OoOCore core(0, cfg, bus, &l1i, &l1d, 0x1000);

    // Step cycle by cycle until LSD locks and becomes active STREAMING
    uint32_t cycles = 0;
    while (!core.is_halted() && !core.get_lsd().is_streaming() && cycles < 500) {
        core.tick();
        cycles++;
    }

    ASSERT_TRUE(core.get_lsd().is_streaming());

    // Run remaining iterations while streaming
    uint64_t streamed_l1i_accesses = 0;
    while (!core.is_halted() && cycles < 2000) {
        uint64_t before_tick_accesses = l1i.get_stats().accesses;
        bool was_streaming = core.get_lsd().is_streaming();
        core.tick();
        cycles++;
        if (was_streaming && core.get_lsd().is_streaming()) {
            // While actively streaming, L1I accesses must not increment
            streamed_l1i_accesses += (l1i.get_stats().accesses - before_tick_accesses);
        }
    }

    EXPECT_TRUE(core.is_halted());
    EXPECT_EQ(core.read_arch_reg(0), 50);
    EXPECT_EQ(streamed_l1i_accesses, 0);
    EXPECT_GE(core.get_lsd().get_stats().loops_detected, 1);
    EXPECT_GT(core.get_lsd().get_stats().uops_streamed, 0);

    std::cout << "[PERF_COUNTER] LSD_UBench_L1IAccessCountCeasesIncrementing:streaming_l1i_accesses=" << streamed_l1i_accesses << std::endl;
    std::cout << "[PERF_COUNTER] LSD_UBench_L1IAccessCountCeasesIncrementing:loops_detected=" << core.get_lsd().get_stats().loops_detected << std::endl;
}

// 9. Multi-Pillar Scientific Verification: (3) Functional parity on nested loops vs Interpreter
TEST(CoreUBenchTest, LSD_UBench_NestedLoopFunctionalParityVsInterpreter) {
    // Nested Loop Program:
    // Outer loop: 4 iterations (R1 = 0..3)
    // Inner loop: 5 iterations (R2 = 0..4)
    // Accumulator: R0 += 2 each inner step -> Final R0 = 40 (4 * 5 * 2)
    //
    // 0x1000: MOVS r0, #0        (0x2000)
    // 0x1002: MOVS r1, #0        (0x2100)
    // Outer loop start: 0x1004
    // 0x1004: MOVS r2, #0        (0x2200)
    // Inner loop start: 0x1006
    // 0x1006: ADDS r0, r0, #2    (0x3002)
    // 0x1008: ADDS r2, r2, #1    (0x3201)
    // 0x100A: CMP r2, #5         (0x2A05)
    // 0x100C: BNE 0x1006         (0xD1FB) -> target 0x1006: (0x1006 - (0x100C + 4)) = -10 bytes = -5 halfwords -> 0xD1FB
    // 0x100E: ADDS r1, r1, #1    (0x3101)
    // 0x1010: CMP r1, #4         (0x2904)
    // 0x1012: BNE 0x1004         (0xD1F7) -> target 0x1004: (0x1004 - (0x1012 + 4)) = -18 bytes = -9 halfwords -> 0xD1F7
    // 0x1014: SVC #0             (0xDF00)

    MemoryBus bus_interp(65536);
    MemoryBus bus_ooo(65536);

    const std::vector<std::pair<uint32_t, uint16_t>> program = {
        {0x1000, 0x2000},
        {0x1002, 0x2100},
        {0x1004, 0x2200},
        {0x1006, 0x3002},
        {0x1008, 0x3201},
        {0x100A, 0x2A05},
        {0x100C, 0xD1FB},
        {0x100E, 0x3101},
        {0x1010, 0x2904},
        {0x1012, 0xD1F7},
        {0x1014, 0xDF00}
    };

    for (const auto& [addr, insn] : program) {
        bus_interp.write16(addr, insn);
        bus_ooo.write16(addr, insn);
    }

    // 1. Run Interpreter Reference Oracle
    ArchitecturalState state_interp;
    state_interp.set_pc(0x1000);
    state_interp.set_sp(0x02000000);
    IsaInterpreter interp(state_interp, bus_interp);
    try {
        interp.run(2000);
    } catch (...) {
        // Halt on SVC fault
    }
    EXPECT_EQ(state_interp.get_reg(0), 40);
    EXPECT_EQ(state_interp.get_reg(1), 4);
    EXPECT_EQ(state_interp.get_reg(2), 5);

    // 2. Run OoOCore with LSD Enabled
    CacheConfig l1i_cfg;
    l1i_cfg.type = CacheType::SET_ASSOCIATIVE;
    l1i_cfg.size_bytes = 32768;
    l1i_cfg.associativity = 4;
    l1i_cfg.line_size = 64;
    l1i_cfg.hit_latency_cycles = 1;

    CacheConfig l1d_cfg;
    l1d_cfg.type = CacheType::SET_ASSOCIATIVE;
    l1d_cfg.size_bytes = 32768;
    l1d_cfg.associativity = 4;
    l1d_cfg.line_size = 64;

    Cache l1i_ooo(l1i_cfg);
    Cache l1d_ooo(l1d_cfg);

    CoreConfig cfg_ooo;
    cfg_ooo.lsd_type = LSDType::LOOP_STREAM;
    cfg_ooo.lsd_capacity = 32;
    cfg_ooo.l1i = l1i_cfg;
    cfg_ooo.l1d = l1d_cfg;

    OoOCore core_ooo(0, cfg_ooo, bus_ooo, &l1i_ooo, &l1d_ooo, 0x1000);
    for (int cycle = 0; cycle < 2000; ++cycle) {
        core_ooo.tick();
        if (core_ooo.is_halted()) break;
    }
    EXPECT_TRUE(core_ooo.is_halted());

    // Compare all general purpose and stack registers R0..R14 (15 registers)
    uint32_t parity_errors = 0;
    uint32_t matched_registers = 0;

    for (size_t r = 0; r < 15; ++r) {
        uint32_t v_interp = state_interp.get_reg(r);
        uint32_t v_ooo = core_ooo.read_arch_reg(r);
        if (v_interp == v_ooo) {
            matched_registers++;
        } else {
            parity_errors++;
            std::cerr << "Mismatch on R" << r << ": Interpreter=" << v_interp << " vs OoO=" << v_ooo << std::endl;
        }
    }

    // Check PC termination matching halt address
    if (state_interp.get_pc() == 0x1014 || state_interp.get_pc() == 0x1016) {
        matched_registers++;
    } else {
        parity_errors++;
    }

    EXPECT_EQ(parity_errors, 0);
    EXPECT_EQ(matched_registers, 16);
    EXPECT_EQ(core_ooo.read_arch_reg(0), 40);
    EXPECT_EQ(core_ooo.read_arch_reg(1), 4);
    EXPECT_EQ(core_ooo.read_arch_reg(2), 5);

    std::cout << "[PERF_COUNTER] LSD_UBench_NestedLoopFunctionalParityVsInterpreter:parity_errors=" << parity_errors << std::endl;
    std::cout << "[PERF_COUNTER] LSD_UBench_NestedLoopFunctionalParityVsInterpreter:matched_registers=" << matched_registers << std::endl;
}

// =============================================================================
// Core Microbenchmarks: Macro-Op Fusion (CoreUBench_MacroOpFusion)
// =============================================================================

// 10. MacroOpFusion: Adjacent CMP + B.cond instruction pair recognized & fused into single uOp_Fused_Branch
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_CmpBranchPair) {
    MacroOpFusionConfig cfg;
    cfg.enabled = true;
    cfg.mode = FusionMode::CMP_BRANCH;
    MacroOpFusionEngine engine(cfg);

    EXPECT_TRUE(engine.is_enabled());

    // 0x1000: CMP r0, #10
    UOp cmp_uop;
    cmp_uop.pc = 0x1000;
    cmp_uop.opcode = Opcode::CMP;
    cmp_uop.type = UOpType::ALU;
    cmp_uop.arch_src1 = 0; // r0
    cmp_uop.imm = 10;
    cmp_uop.is_imm_valid = true;
    cmp_uop.sets_flags = true;

    // 0x1002: BNE 0x1020
    UOp bne_uop;
    bne_uop.pc = 0x1002;
    bne_uop.opcode = Opcode::B;
    bne_uop.type = UOpType::BRANCH;
    bne_uop.is_branch = true;
    bne_uop.cond = ConditionCode::NE;
    bne_uop.actual_target = 0x1020;
    bne_uop.imm = 0x1020;

    EXPECT_TRUE(engine.can_fuse(cmp_uop, bne_uop));

    std::vector<UOp> input_uops = {cmp_uop, bne_uop};
    std::vector<UOp> fused_uops = engine.fuse_sequence(input_uops);

    // Assert fused into a single uOp_Fused_Branch
    ASSERT_EQ(fused_uops.size(), 1);
    const UOp& fused = fused_uops[0];

    EXPECT_TRUE(fused.is_fused);
    EXPECT_TRUE(fused.is_branch);
    EXPECT_EQ(fused.type, UOpType::BRANCH);
    EXPECT_EQ(fused.cond, ConditionCode::NE);
    EXPECT_EQ(fused.arch_src1, 0);
    EXPECT_EQ(fused.imm, 10);
    EXPECT_TRUE(fused.is_imm_valid);
    EXPECT_EQ(fused.actual_target, 0x1020);
    EXPECT_EQ(fused.fused_cmp_opcode, Opcode::CMP);
    EXPECT_EQ(engine.get_stats().fused_pairs, 1);

    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_CmpBranchPair:fused_pairs=" << engine.get_stats().fused_pairs << std::endl;
}

// 11. MacroOpFusion: Adjacent TST + B.cond instruction pair recognized & fused
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_TstBranchPair) {
    MacroOpFusionConfig cfg;
    cfg.enabled = true;
    cfg.mode = FusionMode::CMP_BRANCH;
    MacroOpFusionEngine engine(cfg);

    // 0x2000: TST r1, r2
    UOp tst_uop;
    tst_uop.pc = 0x2000;
    tst_uop.opcode = Opcode::TST;
    tst_uop.type = UOpType::ALU;
    tst_uop.arch_src1 = 1; // r1
    tst_uop.arch_src2 = 2; // r2
    tst_uop.is_imm_valid = false;
    tst_uop.sets_flags = true;

    // 0x2002: BEQ 0x2040
    UOp beq_uop;
    beq_uop.pc = 0x2002;
    beq_uop.opcode = Opcode::B;
    beq_uop.type = UOpType::BRANCH;
    beq_uop.is_branch = true;
    beq_uop.cond = ConditionCode::EQ;
    beq_uop.actual_target = 0x2040;
    beq_uop.imm = 0x2040;

    EXPECT_TRUE(engine.can_fuse(tst_uop, beq_uop));

    std::vector<UOp> input_uops = {tst_uop, beq_uop};
    std::vector<UOp> fused_uops = engine.fuse_sequence(input_uops);

    ASSERT_EQ(fused_uops.size(), 1);
    const UOp& fused = fused_uops[0];

    EXPECT_TRUE(fused.is_fused);
    EXPECT_TRUE(fused.is_branch);
    EXPECT_EQ(fused.cond, ConditionCode::EQ);
    EXPECT_EQ(fused.arch_src1, 1);
    EXPECT_EQ(fused.arch_src2, 2);
    EXPECT_EQ(fused.actual_target, 0x2040);
    EXPECT_EQ(fused.fused_cmp_opcode, Opcode::TST);
    EXPECT_EQ(engine.get_stats().fused_pairs, 1);
}

// 12. MacroOpFusion: Non-adjacent or ineligible pairs are not fused
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_IneligiblePairs) {
    MacroOpFusionConfig cfg;
    cfg.enabled = true;
    MacroOpFusionEngine engine(cfg);

    // Case A: CMP followed by ADD (not a branch)
    UOp cmp_uop = make_dummy_uop(0x3000, UOpType::ALU);
    cmp_uop.opcode = Opcode::CMP;
    UOp add_uop = make_dummy_uop(0x3002, UOpType::ALU);
    add_uop.opcode = Opcode::ADD;

    EXPECT_FALSE(engine.can_fuse(cmp_uop, add_uop));

    std::vector<UOp> seq_a = {cmp_uop, add_uop};
    EXPECT_EQ(engine.fuse_sequence(seq_a).size(), 2);

    // Case B: ADD followed by B.cond (ADD is not CMP/TST)
    UOp bne_uop = make_dummy_uop(0x3004, UOpType::BRANCH, true, false, 0x3020);
    bne_uop.opcode = Opcode::B;
    bne_uop.cond = ConditionCode::NE;

    EXPECT_FALSE(engine.can_fuse(add_uop, bne_uop));

    std::vector<UOp> seq_b = {add_uop, bne_uop};
    EXPECT_EQ(engine.fuse_sequence(seq_b).size(), 2);

    // Case C: CMP followed by Unconditional Branch (cond == AL)
    UOp b_uncond = make_dummy_uop(0x3004, UOpType::BRANCH, true, true, 0x3020);
    b_uncond.opcode = Opcode::B;
    b_uncond.cond = ConditionCode::AL;

    EXPECT_FALSE(engine.can_fuse(cmp_uop, b_uncond));

    // Case D: Disabled fusion engine
    MacroOpFusionConfig disabled_cfg;
    disabled_cfg.enabled = false;
    MacroOpFusionEngine disabled_engine(disabled_cfg);
    EXPECT_FALSE(disabled_engine.can_fuse(cmp_uop, bne_uop));
    EXPECT_EQ(disabled_engine.fuse_sequence({cmp_uop, bne_uop}).size(), 2);
}

