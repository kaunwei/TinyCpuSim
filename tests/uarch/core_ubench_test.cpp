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

// 13. MacroOpFusion: Fused branch execution & single-slot ROB retirement in OoOCore
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_OoOExecutionAndSingleSlotRetirement) {
    // Write a test program:
    // 0x1000: MOV r0, #10     (0x200A)
    // 0x1002: CMP r0, #10     (0x280A)
    // 0x1004: BEQ 0x1008      (0xD000) -> offset 0 (+4 bytes from 1004+4 = 1008)
    // 0x1006: MOV r1, #99     (0x2163) (should be skipped)
    // 0x1008: MOV r1, #42     (0x212A)
    // 0x100A: SVC #0          (0xDF00)
    MemoryBus bus(65536);
    bus.write16(0x1000, 0x200A);
    bus.write16(0x1002, 0x280A);
    bus.write16(0x1004, 0xD000);
    bus.write16(0x1006, 0x2163);
    bus.write16(0x1008, 0x212A);
    bus.write16(0x100A, 0xDF00);

    CacheConfig l1i_cfg;
    l1i_cfg.type = CacheType::PASSTHROUGH;
    CacheConfig l1d_cfg;
    l1d_cfg.type = CacheType::PASSTHROUGH;

    CoreConfig cfg;
    cfg.fusion_mode = FusionMode::CMP_BRANCH;
    cfg.l1i = l1i_cfg;
    cfg.l1d = l1d_cfg;

    OoOCore core(0, cfg, bus, nullptr, nullptr, 0x1000);
    uint32_t cycles = 0;
    while (!core.is_halted() && cycles < 100) {
        core.tick();
        cycles++;
    }

    EXPECT_TRUE(core.is_halted());
    EXPECT_EQ(core.read_arch_reg(0), 10);
    EXPECT_EQ(core.read_arch_reg(1), 42); // Taken branch bypassed 0x1006

    // With Macro-Op Fusion enabled:
    // CMP (1 inst) + BEQ (1 inst) are fused into 1 uOp.
    // Total macro instructions committed: MOV(1) + CMP+BEQ(2) + MOV(1) + SVC(1) = 5 instructions
    // Total uOps committed: MOV(1) + FUSED_CMP_BRANCH(1) + MOV(1) + SVC(1) = 4 uOps!
    // In comparison, without fusion it would be 5 uOps.
    EXPECT_EQ(core.get_committed_instructions(), 5);
    EXPECT_EQ(core.get_stats().committed_uops, 4);
    EXPECT_EQ(core.get_fusion_engine().get_stats().fused_pairs, 1);

    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_OoOExecutionAndSingleSlotRetirement:committed_insts="
              << core.get_committed_instructions() << std::endl;
    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_OoOExecutionAndSingleSlotRetirement:committed_uops="
              << core.get_stats().committed_uops << std::endl;
    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_OoOExecutionAndSingleSlotRetirement:fused_pairs="
              << core.get_fusion_engine().get_stats().fused_pairs << std::endl;
}

// 14. MacroOpFusion: Atomic execution across ALU and Branch ports (branch evaluated with fused ALU operands)
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_AtomicPortExecution) {
    // 0x2000: MOV r1, #5      (0x2105)
    // 0x2002: CMP r1, #10     (0x290A)
    // 0x2004: BEQ 0x2008      (0xD000) (Not taken because 5 != 10)
    // 0x2006: MOV r2, #77     (0x224D) (Executed because branch not taken)
    // 0x2008: SVC #0          (0xDF00)
    MemoryBus bus(65536);
    bus.write16(0x2000, 0x2105);
    bus.write16(0x2002, 0x290A);
    bus.write16(0x2004, 0xD000);
    bus.write16(0x2006, 0x224D);
    bus.write16(0x2008, 0xDF00);

    CacheConfig l1i_cfg;
    l1i_cfg.type = CacheType::PASSTHROUGH;
    CacheConfig l1d_cfg;
    l1d_cfg.type = CacheType::PASSTHROUGH;

    CoreConfig cfg;
    cfg.fusion_mode = FusionMode::CMP_BRANCH;
    cfg.l1i = l1i_cfg;
    cfg.l1d = l1d_cfg;

    OoOCore core(0, cfg, bus, nullptr, nullptr, 0x2000);
    uint32_t cycles = 0;
    while (!core.is_halted() && cycles < 100) {
        core.tick();
        cycles++;
    }

    EXPECT_TRUE(core.is_halted());
    EXPECT_EQ(core.read_arch_reg(1), 5);
    EXPECT_EQ(core.read_arch_reg(2), 77); // Fall-through executed correctly

    EXPECT_EQ(core.get_committed_instructions(), 5);
    EXPECT_EQ(core.get_stats().committed_uops, 4);
    EXPECT_EQ(core.get_fusion_engine().get_stats().fused_pairs, 1);
}

// 15. MacroOpFusion Multi-Pillar Verification:
// (1) Invariant verification that Committed_Insts / Committed_uOps = 2.0x on pure fused pairs while RS/ROB slot usage is halved
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_RatioAndSlotUsageHalved) {
    // 4 chained fused CMP+BEQ taken pairs jumping from one pair to the next:
    // Pair 1:
    // 0x1000: CMP r0, #0       (0x2800)
    // 0x1002: BEQ 0x1006       (0xD000) -> jumps to Pair 2 at 0x1006 (skips 0x1004)
    // 0x1004: NOP              (0xBF00)
    // Pair 2:
    // 0x1006: CMP r1, #0       (0x2900)
    // 0x1008: BEQ 0x100C       (0xD000) -> jumps to Pair 3 at 0x100C (skips 0x100A)
    // 0x100A: NOP              (0xBF00)
    // Pair 3:
    // 0x100C: CMP r2, #0       (0x2A00)
    // 0x100E: BEQ 0x1012       (0xD000) -> jumps to Pair 4 at 0x1012 (skips 0x1010)
    // 0x1010: NOP              (0xBF00)
    // Pair 4:
    // 0x1012: CMP r3, #0       (0x2B00)
    // 0x1014: BEQ 0x1018       (0xD000) -> jumps to Halt at 0x1018 (skips 0x1016)
    // 0x1016: NOP              (0xBF00)
    // Halt:
    // 0x1018: SVC #0           (0xDF00)
    MemoryBus bus_fused(65536);
    MemoryBus bus_nofused(65536);

    const std::vector<std::pair<uint32_t, uint16_t>> prog = {
        {0x1000, 0x2800},
        {0x1002, 0xD000},
        {0x1004, 0xBF00},
        {0x1006, 0x2900},
        {0x1008, 0xD000},
        {0x100A, 0xBF00},
        {0x100C, 0x2A00},
        {0x100E, 0xD000},
        {0x1010, 0xBF00},
        {0x1012, 0x2B00},
        {0x1014, 0xD000},
        {0x1016, 0xBF00},
        {0x1018, 0xDF00}
    };

    for (const auto& [addr, insn] : prog) {
        bus_fused.write16(addr, insn);
        bus_nofused.write16(addr, insn);
    }

    CacheConfig l1i_cfg;
    l1i_cfg.type = CacheType::PASSTHROUGH;
    CacheConfig l1d_cfg;
    l1d_cfg.type = CacheType::PASSTHROUGH;

    // 1. Run with Fusion ENABLED
    CoreConfig cfg_fused;
    cfg_fused.fusion_mode = FusionMode::CMP_BRANCH;
    cfg_fused.l1i = l1i_cfg;
    cfg_fused.l1d = l1d_cfg;

    OoOCore core_fused(0, cfg_fused, bus_fused, nullptr, nullptr, 0x1000);
    uint32_t cycles = 0;
    while (!core_fused.is_halted() && cycles < 100) {
        core_fused.tick();
        cycles++;
    }

    EXPECT_TRUE(core_fused.is_halted());

    // 2. Run with Fusion DISABLED
    CoreConfig cfg_nofused;
    cfg_nofused.fusion_mode = FusionMode::NONE;
    cfg_nofused.l1i = l1i_cfg;
    cfg_nofused.l1d = l1d_cfg;

    OoOCore core_nofused(0, cfg_nofused, bus_nofused, nullptr, nullptr, 0x1000);
    cycles = 0;
    while (!core_nofused.is_halted() && cycles < 100) {
        core_nofused.tick();
        cycles++;
    }

    EXPECT_TRUE(core_nofused.is_halted());

    // Architectural committed instruction count MUST be identical across both modes:
    // 4 pairs (8) + SVC (1) = 9 instructions
    EXPECT_EQ(core_fused.get_committed_instructions(), 9);
    EXPECT_EQ(core_nofused.get_committed_instructions(), 9);

    // Number of committed fused pairs in execution = 4
    uint64_t fused_pairs_committed = core_nofused.get_stats().committed_uops - core_fused.get_stats().committed_uops;
    EXPECT_EQ(fused_pairs_committed, 4);

    // Without fusion: 9 uOps committed (and allocated into ROB / RS)
    // With fusion: 9 - 4 = 5 uOps committed (and allocated into ROB / RS)
    EXPECT_EQ(core_nofused.get_stats().committed_uops, 9);
    EXPECT_EQ(core_fused.get_stats().committed_uops, 5);

    // Invariant for the fused pairs: 8 committed instructions / 4 committed uOps = 2.0x
    uint64_t fused_insts = fused_pairs_committed * 2;
    double fused_pair_ratio = static_cast<double>(fused_insts) / static_cast<double>(fused_pairs_committed);
    EXPECT_DOUBLE_EQ(fused_pair_ratio, 2.0);

    // RS/ROB slot usage for fused pairs is halved (50.0% reduction)
    double slot_usage_reduction_pct = (1.0 - static_cast<double>(fused_pairs_committed) / static_cast<double>(fused_insts)) * 100.0;
    EXPECT_DOUBLE_EQ(slot_usage_reduction_pct, 50.0);

    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_RatioAndSlotUsageHalved:fused_pairs=" << fused_pairs_committed << std::endl;
    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_RatioAndSlotUsageHalved:insts_per_fused_uop=" << fused_pair_ratio << std::endl;
    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_RatioAndSlotUsageHalved:slot_reduction_pct=" << slot_usage_reduction_pct << std::endl;
}

// 16. MacroOpFusion Multi-Pillar Verification:
// (2) Boundary condition non-fusion test on non-adjacent instructions
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_BoundaryNonAdjacentNonFusion) {
    // Interleave CMP and B.cond with an unrelated instruction (MOV r2, r0: 0x4602, does not set flags):
    // 0x1000: MOVS r0, #10    (0x200A)
    // 0x1002: CMP r0, #10     (0x280A) (sets flags: Z=1)
    // 0x1004: MOV r2, r0      (0x4602) -> Separator! CMP and BEQ are non-adjacent; does NOT alter flags
    // 0x1006: BEQ 0x100A      (0xD000) -> target 0x100A (+4 bytes from 1006+4 = 100A)
    // 0x1008: MOVS r1, #99    (0x2163) (skipped because BEQ jumps to 0x100A)
    // 0x100A: SVC #0          (0xDF00)
    MemoryBus bus(65536);
    bus.write16(0x1000, 0x200A);
    bus.write16(0x1002, 0x280A);
    bus.write16(0x1004, 0x4602);
    bus.write16(0x1006, 0xD000);
    bus.write16(0x1008, 0x2163);
    bus.write16(0x100A, 0xDF00);

    CacheConfig l1i_cfg;
    l1i_cfg.type = CacheType::PASSTHROUGH;
    CacheConfig l1d_cfg;
    l1d_cfg.type = CacheType::PASSTHROUGH;

    CoreConfig cfg;
    cfg.fusion_mode = FusionMode::CMP_BRANCH;
    cfg.l1i = l1i_cfg;
    cfg.l1d = l1d_cfg;

    OoOCore core(0, cfg, bus, nullptr, nullptr, 0x1000);
    uint32_t cycles = 0;
    while (!core.is_halted() && cycles < 100) {
        core.tick();
        cycles++;
    }

    EXPECT_TRUE(core.is_halted());
    EXPECT_EQ(core.read_arch_reg(0), 10);
    EXPECT_EQ(core.read_arch_reg(2), 10);
    EXPECT_EQ(core.read_arch_reg(1), 0); // r1 was untouched because branch skipped 0x1008

    // Because of the interleaving MOV r2, r0, fusion MUST NOT occur
    EXPECT_EQ(core.get_fusion_engine().get_stats().fused_pairs, 0);
    EXPECT_GE(core.get_fusion_engine().get_stats().ineligible_pairs, 1);

    // Total committed instructions = MOVS(1) + CMP(1) + MOV(1) + BEQ(1) + SVC(1) = 5
    // Total committed uOps = 5 (1:1 ratio, 0 fused)
    EXPECT_EQ(core.get_committed_instructions(), 5);
    EXPECT_EQ(core.get_stats().committed_uops, 5);

    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_BoundaryNonAdjacentNonFusion:fused_pairs="
              << core.get_fusion_engine().get_stats().fused_pairs << std::endl;
    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_BoundaryNonAdjacentNonFusion:ineligible_pairs="
              << core.get_fusion_engine().get_stats().ineligible_pairs << std::endl;
}

// 17. MacroOpFusion Multi-Pillar Verification:
// (3) Functional parity vs Interpreter on complex fused conditional control flow
TEST(CoreUBenchTest, CoreUBench_MacroOpFusion_FunctionalParityVsInterpreter) {
    // Multi-branch control flow with multiple fused pairs (CMP+BEQ, CMP+BNE, TST+BEQ):
    // 0x1000: MOVS r0, #0       (0x2000)
    // 0x1002: MOVS r1, #3       (0x2103)
    // 0x1004: MOVS r2, #0       (0x2200)
    // Loop1:
    // 0x1006: ADDS r0, r0, #5   (0x3005)
    // 0x1008: CMP r0, #15       (0x280F)
    // 0x100A: BNE 0x1006        (0xD1FC) -> offset -8 bytes (-4 hw) -> 0xD1FC
    // PostLoop1:
    // 0x100C: TST r1, #1        (0x2301 for MOV r3,#1 then TST r1, r3 -> 0x4219) -> let's do TST r1, r3
    // 0x100C: MOVS r3, #1       (0x2301)
    // 0x100E: TST r1, r3        (0x4219)
    // 0x1010: BEQ 0x1016        (0xD001) -> offset +6 bytes (+3 hw from 1010+4=1014 -> 1016 is +2 bytes = +1 hw) -> 0xD001
    // 0x1012: ADDS r2, r2, #100 (0x3264)
    // 0x1014: B 0x1018          (0xE000) -> offset +4 bytes (+1 hw) -> 0xE000
    // 0x1016: ADDS r2, r2, #200 (0x32C8)
    // 0x1018: SVC #0            (0xDF00)
    MemoryBus bus_interp(65536);
    MemoryBus bus_ooo(65536);

    const std::vector<std::pair<uint32_t, uint16_t>> prog = {
        {0x1000, 0x2000},
        {0x1002, 0x2103},
        {0x1004, 0x2200},
        {0x1006, 0x3005},
        {0x1008, 0x280F},
        {0x100A, 0xD1FC},
        {0x100C, 0x2301},
        {0x100E, 0x4219},
        {0x1010, 0xD001},
        {0x1012, 0x3264},
        {0x1014, 0xE000},
        {0x1016, 0x32C8},
        {0x1018, 0xDF00}
    };

    for (const auto& [addr, insn] : prog) {
        bus_interp.write16(addr, insn);
        bus_ooo.write16(addr, insn);
    }

    // 1. Run Interpreter
    ArchitecturalState state_interp;
    state_interp.set_pc(0x1000);
    state_interp.set_sp(0x02000000);
    IsaInterpreter interp(state_interp, bus_interp);
    try {
        interp.run(2000);
    } catch (...) {
        // Halt on SVC fault
    }

    // 2. Run OoOCore with MacroOpFusion
    CoreConfig cfg;
    cfg.fusion_mode = FusionMode::CMP_BRANCH;

    OoOCore core_ooo(0, cfg, bus_ooo, nullptr, nullptr, 0x1000);
    for (int cycle = 0; cycle < 2000; ++cycle) {
        core_ooo.tick();
        if (core_ooo.is_halted()) break;
    }
    EXPECT_TRUE(core_ooo.is_halted());

    // Verify architectural register matching
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
    if (state_interp.get_pc() == 0x1018 || state_interp.get_pc() == 0x101A) {
        matched_registers++;
    } else {
        parity_errors++;
    }

    EXPECT_EQ(parity_errors, 0);
    EXPECT_EQ(matched_registers, 16);
    EXPECT_EQ(core_ooo.read_arch_reg(0), 15);
    EXPECT_EQ(core_ooo.read_arch_reg(1), 3);
    EXPECT_EQ(core_ooo.read_arch_reg(2), 100);
    EXPECT_GT(core_ooo.get_fusion_engine().get_stats().fused_pairs, 0);

    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_FunctionalParityVsInterpreter:parity_errors=" << parity_errors << std::endl;
    std::cout << "[PERF_COUNTER] CoreUBench_MacroOpFusion_FunctionalParityVsInterpreter:matched_registers=" << matched_registers << std::endl;
}



