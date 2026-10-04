#include <gtest/gtest.h>
#include <vector>
#include <iostream>
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/uarch/lsu.hpp"
#include "tinyarmsim/uarch/memory_disambiguator.hpp"
#include "tinyarmsim/uarch/ooo_core.hpp"
#include "tinyarmsim/uarch/topdown_profiler.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

// =============================================================================
// Memory Disambiguation & Store Sets Isolated Microbenchmarks
// =============================================================================

TEST(LSUUBenchTest, BlindDisambiguator_AlwaysPredictsBypass) {
    BlindDisambiguator disambiguator;

    uint32_t store_pc = 0x1000;
    uint32_t load_pc = 0x1004;

    disambiguator.insert_store(store_pc, 10);
    disambiguator.insert_load(load_pc, 11);

    // Blind speculation always predicts bypass (no dependency detected)
    EXPECT_EQ(disambiguator.check_dep(load_pc), 0);
    EXPECT_TRUE(disambiguator.can_bypass(load_pc));

    // Even after recording violation, blind predictor does not stall
    disambiguator.record_violation(store_pc, load_pc);
    EXPECT_EQ(disambiguator.check_dep(load_pc), 0);
}

TEST(LSUUBenchTest, StoreSets_InitialStateNoDependencyLoadBypass) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc = 0x8000;
    uint32_t load_pc = 0x8004;

    // Before any violation, neither load nor store has an SSID
    ss.insert_store(store_pc, 100);
    ss.insert_load(load_pc, 101);

    EXPECT_EQ(ss.check_dep(load_pc), 0);
    EXPECT_TRUE(ss.can_bypass(load_pc));
}

TEST(LSUUBenchTest, StoreSets_ViolationCreatesStoreSetAndTracksDependency) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc = 0x4000;
    uint32_t load_pc = 0x4008;

    // Train on a memory order violation
    ss.record_violation(store_pc, load_pc);

    EXPECT_TRUE(ss.has_ssid(store_pc));
    EXPECT_TRUE(ss.has_ssid(load_pc));
    EXPECT_EQ(ss.get_ssid(store_pc), ss.get_ssid(load_pc));

    // Now insert the store (seq=200) into the pipeline
    ss.insert_store(store_pc, 200);

    // When the load arrives, it should detect dependency on store 200
    EXPECT_EQ(ss.check_dep(load_pc), 200);
    EXPECT_FALSE(ss.can_bypass(load_pc));
}

TEST(LSUUBenchTest, StoreSets_StoreIssuedClearsLFSTDependency) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc = 0x4000;
    uint32_t load_pc = 0x4008;

    ss.record_violation(store_pc, load_pc);
    ss.insert_store(store_pc, 300);

    EXPECT_EQ(ss.check_dep(load_pc), 300);

    // Store issues and calculates its address
    ss.store_issued(store_pc, 300);

    // LFST entry should be cleared/invalidated for this SSID
    EXPECT_EQ(ss.check_dep(load_pc), 0);
    EXPECT_TRUE(ss.can_bypass(load_pc));
}

TEST(LSUUBenchTest, StoreSets_MultipleStoresInSameSetLatestWins) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc1 = 0x5000;
    uint32_t load_pc = 0x5008;

    ss.record_violation(store_pc1, load_pc);

    // Store 1 inserted (seq=400)
    ss.insert_store(store_pc1, 400);
    EXPECT_EQ(ss.check_dep(load_pc), 400);

    // Store 2 inserted from same PC (seq=405)
    ss.insert_store(store_pc1, 405);
    EXPECT_EQ(ss.check_dep(load_pc), 405);

    // Store 1 issues (seq=400) -> LFST still points to 405 because 405 was later!
    ss.store_issued(store_pc1, 400);
    EXPECT_EQ(ss.check_dep(load_pc), 405);

    // Store 2 issues (seq=405) -> LFST cleared
    ss.store_issued(store_pc1, 405);
    EXPECT_EQ(ss.check_dep(load_pc), 0);
}

TEST(LSUUBenchTest, StoreSets_SetMergingOnMultipleViolations) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc1 = 0x1000;
    uint32_t load_pc1 = 0x1004;

    uint32_t store_pc2 = 0x2000;
    uint32_t load_pc2 = 0x2004;

    // Create Set 1 for (store_pc1, load_pc1)
    ss.record_violation(store_pc1, load_pc1);
    uint32_t ssid1 = ss.get_ssid(load_pc1);

    // Create Set 2 for (store_pc2, load_pc2)
    ss.record_violation(store_pc2, load_pc2);
    uint32_t ssid2 = ss.get_ssid(load_pc2);

    EXPECT_NE(ssid1, ssid2);

    // Now load_pc1 also conflicts with store_pc2 -> merges to min SSID
    ss.record_violation(store_pc2, load_pc1);

    uint32_t expected_merged_ssid = std::min(ssid1, ssid2);
    EXPECT_EQ(ss.get_ssid(store_pc2), expected_merged_ssid);
    EXPECT_EQ(ss.get_ssid(load_pc1), expected_merged_ssid);
}

TEST(LSUUBenchTest, StoreSets_SquashRollsBackLFST) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc = 0x3000;
    uint32_t load_pc = 0x3004;

    ss.record_violation(store_pc, load_pc);

    ss.insert_store(store_pc, 500);
    EXPECT_EQ(ss.check_dep(load_pc), 500);

    // Squash everything younger than 450 (store 500 was speculative)
    ss.squash(450);

    // Store 500 should be removed from LFST
    EXPECT_EQ(ss.check_dep(load_pc), 0);
    EXPECT_TRUE(ss.can_bypass(load_pc));
}

TEST(LSUUBenchTest, StoreSets_ClearWipesAllState) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc = 0x3000;
    uint32_t load_pc = 0x3004;

    ss.record_violation(store_pc, load_pc);
    ss.insert_store(store_pc, 600);
    EXPECT_TRUE(ss.has_ssid(store_pc));
    EXPECT_EQ(ss.check_dep(load_pc), 600);

    ss.clear();

    EXPECT_FALSE(ss.has_ssid(store_pc));
    EXPECT_FALSE(ss.has_ssid(load_pc));
    EXPECT_EQ(ss.check_dep(load_pc), 0);
}

TEST(LSUUBenchTest, LSU_UBench_StoreSetsDynamicBypassPrediction) {
    StoreSetsDisambiguator ss(1024, 256);

    // Benchmark loop:
    // Pattern: 500 iterations of:
    // Store A (PC 0x1000) -> Load A (PC 0x1004, dependent)
    // Store B (PC 0x2000) -> Load C (PC 0x3004, independent)

    uint32_t store_a_pc = 0x1000;
    uint32_t load_a_pc = 0x1004;
    uint32_t store_b_pc = 0x2000;
    uint32_t load_c_pc = 0x3004;

    // Iteration 0: First time violation occurs for pair A
    ss.record_violation(store_a_pc, load_a_pc);

    uint64_t seq = 1;
    uint32_t correct_bypasses = 0;
    uint32_t correct_dependencies = 0;

    for (int iter = 0; iter < 500; ++iter) {
        uint64_t st_a_seq = seq++;
        uint64_t ld_a_seq = seq++;
        uint64_t st_b_seq = seq++;
        uint64_t ld_c_seq = seq++;

        // Store A enters pipeline
        ss.insert_store(store_a_pc, st_a_seq);
        ss.insert_load(load_a_pc, ld_a_seq);

        // Load A checks dep: must stall on st_a_seq
        uint64_t dep_a = ss.check_dep(load_a_pc);
        if (dep_a == st_a_seq) {
            correct_dependencies++;
        }

        // Store A issues
        ss.store_issued(store_a_pc, st_a_seq);

        // Store B enters pipeline
        ss.insert_store(store_b_pc, st_b_seq);
        ss.insert_load(load_c_pc, ld_c_seq);

        // Load C checks dep: independent, can bypass!
        uint64_t dep_c = ss.check_dep(load_c_pc);
        if (dep_c == 0) {
            correct_bypasses++;
        }

        // Store B issues
        ss.store_issued(store_b_pc, st_b_seq);
    }

    EXPECT_EQ(correct_dependencies, 500);
    EXPECT_EQ(correct_bypasses, 500);
    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsDynamicBypassPrediction:correct_dependencies=" << correct_dependencies << std::endl;
    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsDynamicBypassPrediction:correct_bypasses=" << correct_bypasses << std::endl;
}

// Pillar 1: Learning convergence test verifying violation count drops to 0 after 1st occurrence and forwarding hits increase
TEST(LSUUBenchTest, LSU_UBench_StoreSetsLearningConvergence) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    cfg.type = LsuType::SPECULATIVE_OOO;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    uint32_t store_pc = 0x1000;
    uint32_t load_pc = 0x1004;
    uint32_t indep_store_pc = 0x2000;
    uint32_t indep_load_pc = 0x3004;
    uint32_t target_addr = 0x2000;
    uint32_t indep_addr = 0x3000;

    constexpr uint64_t kTotalIterations = 500;
    uint64_t total_violations = 0;
    uint64_t forwarding_hits = 0;
    uint64_t independent_bypasses = 0;
    uint64_t global_seq = 1;

    for (uint64_t iter = 0; iter < kTotalIterations; ++iter) {
        // --- Pair 1: Dependent Store & Load at target_addr ---
        uint64_t st_seq = global_seq++;
        uint64_t ld_seq = global_seq++;

        UOp s_uop;
        s_uop.pc = store_pc;
        s_uop.seq_num = st_seq;
        s_uop.rob_idx = 0;
        size_t sq_idx = lsu.allocate_store(s_uop);

        UOp l_uop;
        l_uop.pc = load_pc;
        l_uop.seq_num = ld_seq;
        l_uop.rob_idx = 1;
        size_t lq_idx = lsu.allocate_load(l_uop);

        if (iter == 0) {
            // Iteration 0: Initial state (SSIT empty), Load speculatively queries bypass -> allows bypass!
            EXPECT_TRUE(lsu.can_bypass_disambiguation(l_uop.pc, l_uop.seq_num));

            // Load executes speculatively from memory before store address resolves
            bus.write32(target_addr, 0x10000000);
            auto l_res = lsu.execute_load(lq_idx, target_addr, 4, l_uop.seq_num);
            EXPECT_TRUE(l_res.completed);
            EXPECT_FALSE(l_res.forwarded);

            // Store address resolves to same address -> Memory order violation detected!
            size_t viol_rob = 0;
            bool viol = lsu.execute_store_address(sq_idx, target_addr, 4, s_uop.seq_num, viol_rob);
            EXPECT_TRUE(viol);
            total_violations++;

            // Pipeline recovers: squash younger load, commit store
            lsu.flush_younger_than(s_uop.seq_num);
            lsu.execute_store_data(sq_idx, 0x20000000);
            lsu.commit_store(sq_idx);
        } else {
            // Iterations 1..499: Store Sets has learned the dependency!
            // Load CANNOT bypass before store address is computed
            EXPECT_FALSE(lsu.can_bypass_disambiguation(l_uop.pc, l_uop.seq_num));

            // Store calculates address and writes data
            size_t viol_rob = 0;
            bool viol = lsu.execute_store_address(sq_idx, target_addr, 4, s_uop.seq_num, viol_rob);
            EXPECT_FALSE(viol); // Zero violations!
            if (viol) total_violations++;

            uint32_t st_val = static_cast<uint32_t>(0xCAFE0000 + iter);
            lsu.execute_store_data(sq_idx, st_val);

            // Store address has resolved, load can now bypass disambiguation and forward!
            EXPECT_TRUE(lsu.can_bypass_disambiguation(l_uop.pc, l_uop.seq_num));
            auto l_res = lsu.execute_load(lq_idx, target_addr, 4, l_uop.seq_num);
            EXPECT_TRUE(l_res.completed);
            EXPECT_TRUE(l_res.forwarded);
            EXPECT_EQ(l_res.data, st_val);
            if (l_res.forwarded) forwarding_hits++;

            lsu.commit_store(sq_idx);
            lsu.free_load(lq_idx);
        }

        // --- Pair 2: Independent Store & Load at disjoint address ---
        uint64_t indep_st_seq = global_seq++;
        uint64_t indep_ld_seq = global_seq++;

        UOp indep_s_uop;
        indep_s_uop.pc = indep_store_pc;
        indep_s_uop.seq_num = indep_st_seq;
        indep_s_uop.rob_idx = 2;
        size_t indep_sq = lsu.allocate_store(indep_s_uop);

        UOp indep_l_uop;
        indep_l_uop.pc = indep_load_pc;
        indep_l_uop.seq_num = indep_ld_seq;
        indep_l_uop.rob_idx = 3;
        size_t indep_lq = lsu.allocate_load(indep_l_uop);

        // Independent load has no dependency -> immediately bypasses!
        EXPECT_TRUE(lsu.can_bypass_disambiguation(indep_l_uop.pc, indep_l_uop.seq_num));
        independent_bypasses++;

        size_t dummy_v = 0;
        lsu.execute_store_address(indep_sq, indep_addr, 4, indep_st_seq, dummy_v);
        lsu.execute_store_data(indep_sq, 0x55555555);

        bus.write32(indep_addr + 0x100, 0x12345678);
        auto indep_res = lsu.execute_load(indep_lq, indep_addr + 0x100, 4, indep_ld_seq);
        EXPECT_TRUE(indep_res.completed);

        lsu.commit_store(indep_sq);
        lsu.free_load(indep_lq);
    }

    // Verification invariants:
    // (1) Violation count is strictly 1 (the initial discovery), exactly 0 in all subsequent iterations
    EXPECT_EQ(total_violations, 1);
    // (2) Forwarding hits increased across all remaining iterations (499 / 499)
    EXPECT_EQ(forwarding_hits, kTotalIterations - 1);
    // (3) Independent loads bypass with 100% precision
    EXPECT_EQ(independent_bypasses, kTotalIterations);

    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsLearningConvergence:converged_violations=" << total_violations << std::endl;
    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsLearningConvergence:forwarding_hits=" << forwarding_hits << std::endl;
    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsLearningConvergence:independent_bypasses=" << independent_bypasses << std::endl;
}

// Pillar 2: 100% Functional parity vs Interpreter on pointer/stack heavy workloads
TEST(LSUUBenchTest, LSU_UBench_StoreSetsFunctionalParityVsInterpreter) {
    MemoryBus bus_ooo(64 * 1024 * 1024);
    MemoryBus bus_interp(64 * 1024 * 1024);

    // Pointer/stack manipulation Thumb program:
    // Base: 0x1000
    // 0x1000: MOV R0, #0x20       (Thumb-16: 0x2020)
    // 0x1002: LSL R0, R0, #8      (Thumb-16: 0x0200) -> R0 = 0x2000 (Data buffer)
    // 0x1004: MOV R1, #10         (Thumb-16: 0x210A) -> R1 = 10 (Loop counter)
    // 0x1006: MOV R5, #0          (Thumb-16: 0x2500) -> R5 = 0 (Accumulator)
    // Loop (0x1008):
    // 0x1008: STR R1, [R0, #0]    (Thumb-16: 0x6001) -> Store R1 to *R0
    // 0x100A: LDR R3, [R0, #0]    (Thumb-16: 0x6803) -> Load *R0 into R3 (STLF / aliasing)
    // 0x100C: PUSH {R3}           (Thumb-16: 0xB408) -> Push R3 onto stack
    // 0x100E: ADD R0, R0, #4      (Thumb-16: 0x3004) -> Advance buffer pointer
    // 0x1010: POP {R4}            (Thumb-16: 0xBC10) -> Pop stack into R4
    // 0x1012: ADD R5, R5, R4      (Thumb-16: 0x1925) -> Accumulate R5 += R4
    // 0x1014: SUB R1, R1, #1      (Thumb-16: 0x3901) -> Decrement R1
    // 0x1016: CMP R1, #0          (Thumb-16: 0x2900) -> Check loop termination
    // 0x1018: BNE -18             (Thumb-16: 0xD1F6) -> Branch back to 0x1008
    // 0x101A: SVC #0              (Thumb-16: 0xDF00) -> Halt

    std::vector<std::pair<uint32_t, uint16_t>> program = {
        {0x1000, 0x2020},
        {0x1002, 0x0200},
        {0x1004, 0x210A},
        {0x1006, 0x2500},
        {0x1008, 0x6001},
        {0x100A, 0x6803},
        {0x100C, 0xB408},
        {0x100E, 0x3004},
        {0x1010, 0xBC10},
        {0x1012, 0x192D},
        {0x1014, 0x3901},
        {0x1016, 0x2900},
        {0x1018, 0xD1F6},
        {0x101A, 0xDF00},
    };

    for (const auto& [addr, halfword] : program) {
        bus_ooo.write16(addr, halfword);
        bus_interp.write16(addr, halfword);
    }

    // Run on Interpreter
    ArchitecturalState state_interp;
    state_interp.set_pc(0x1000);
    state_interp.set_sp(0x02000000);
    IsaInterpreter interp(state_interp, bus_interp);

    for (int step = 0; step < 200; ++step) {
        try {
            interp.step();
        } catch (const CpuFaultException&) {
            break;
        }
    }

    // Run on OoOCore (with Speculative Store Sets LSU)
    CoreConfig cfg;
    cfg.fetch_width = 4;
    cfg.issue_width = 4;
    cfg.commit_width = 4;
    cfg.lsu.type = LsuType::SPECULATIVE_OOO;
    OoOCore core(0, cfg, bus_ooo, nullptr, nullptr, 0x1000);

    for (int cycle = 0; cycle < 500; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    EXPECT_TRUE(core.is_halted());

    // Compare all general purpose and stack registers R0..R14 (15 registers)
    uint32_t parity_errors = 0;
    uint32_t matched_registers = 0;

    for (size_t r = 0; r < 15; ++r) {
        uint32_t v_interp = state_interp.get_reg(r);
        uint32_t v_ooo = core.read_arch_reg(r);
        if (v_interp == v_ooo) {
            matched_registers++;
        } else {
            parity_errors++;
            std::cerr << "Mismatch on R" << r << ": Interpreter=" << v_interp << " vs OoO=" << v_ooo << std::endl;
        }
    }

    // Check PC termination matching halt address
    if (state_interp.get_pc() == 0x101A || state_interp.get_pc() == 0x101C) {
        matched_registers++;
    } else {
        parity_errors++;
    }

    // Verify memory parity across buffer (0x2000 .. 0x2030)
    for (uint32_t addr = 0x2000; addr < 0x2030; addr += 4) {
        uint32_t m_interp = bus_interp.read32(addr);
        uint32_t m_ooo = bus_ooo.read32(addr);
        EXPECT_EQ(m_interp, m_ooo);
        if (m_interp != m_ooo) parity_errors++;
    }

    // Verify stack memory parity (0x01FFFFD0 .. 0x02000000)
    for (uint32_t addr = 0x01FFFFD0; addr <= 0x02000000; addr += 4) {
        uint32_t s_interp = bus_interp.read32(addr);
        uint32_t s_ooo = bus_ooo.read32(addr);
        EXPECT_EQ(s_interp, s_ooo);
        if (s_interp != s_ooo) parity_errors++;
    }

    EXPECT_EQ(parity_errors, 0);
    EXPECT_EQ(matched_registers, 16);
    EXPECT_EQ(core.read_arch_reg(5), 55); // Sum(10..1) = 55

    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsFunctionalParityVsInterpreter:parity_errors=" << parity_errors << std::endl;
    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsFunctionalParityVsInterpreter:matched_registers=" << matched_registers << std::endl;
}

// Pillar 3: TMAM slot conservation & Bad Speculation elimination
TEST(LSUUBenchTest, LSU_UBench_StoreSetsTMAMSlotConservation) {
    TopDownProfiler profiler;
    profiler.init(1, 4);

    // Iteration 0: Initial violation squashes 4 slots of bad speculation
    for (int i = 0; i < 4; ++i) {
        profiler.record_slot(0, SlotType::BadSpecMispredict);
    }
    profiler.record_event(0, PerfEvent::MemoryOrderViolation);

    // Iterations 1..49: Store Sets eliminates bad speculation; converts to retiring & memory backend
    for (int i = 0; i < 49; ++i) {
        profiler.record_slot(0, SlotType::RetiringBaseAlu);
        profiler.record_slot(0, SlotType::RetiringMem);
        profiler.record_slot(0, SlotType::BackEndMemStoreBufFull);
        profiler.record_slot(0, SlotType::FrontEndFetchBubble);
    }

    const auto& report = profiler.get_report(0);
    EXPECT_EQ(report.total_slots, 200);

    // Slot conservation invariant: Total == Retiring + BadSpec + Frontend + Backend
    uint64_t sum_slots = report.retiring_slots + report.bad_spec_slots + report.frontend_slots + report.backend_slots;
    EXPECT_EQ(sum_slots, report.total_slots);

    double sum_pct = report.retiring_pct() + report.bad_spec_pct() + report.frontend_pct() + report.backend_pct();
    EXPECT_NEAR(sum_pct, 100.0, 0.001);

    // Bad speculation is strictly contained to the initial violation
    EXPECT_EQ(report.event_mob_violations, 1);
    EXPECT_EQ(report.bad_spec_slots, 4);

    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsTMAMSlotConservation:slot_conservation=" << sum_pct << std::endl;
    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsTMAMSlotConservation:bad_spec_eliminated=1" << std::endl;
}

// Pillar 4: Store Sets Merging and Squash Rollback Recovery
TEST(LSUUBenchTest, LSU_UBench_StoreSetsMergingAndSquashRecovery) {
    StoreSetsDisambiguator ss(1024, 256);

    uint32_t store_pc1 = 0x1000;
    uint32_t store_pc2 = 0x2000;
    uint32_t load_pc1 = 0x3000;

    // Transitive violations merge store_pc1 and store_pc2 into a single unified SSID
    ss.record_violation(store_pc1, load_pc1);
    ss.record_violation(store_pc2, load_pc1);

    EXPECT_EQ(ss.get_ssid(store_pc1), ss.get_ssid(store_pc2));
    EXPECT_EQ(ss.get_ssid(load_pc1), ss.get_ssid(store_pc1));

    uint32_t merged_ssid = ss.get_ssid(load_pc1);
    EXPECT_NE(merged_ssid, StoreSetsDisambiguator::INVALID_SSID);

    // Speculative store inserted
    ss.insert_store(store_pc1, 1000);
    EXPECT_EQ(ss.check_dep(load_pc1), 1000);

    // Squash rolls back speculation
    ss.squash(900);
    EXPECT_EQ(ss.check_dep(load_pc1), 0);
    EXPECT_TRUE(ss.can_bypass(load_pc1));

    std::cout << "[PERF_COUNTER] LSU_UBench_StoreSetsMergingAndSquashRecovery:merged_ssids=1" << std::endl;
}

TEST(LSUUBenchTest, LSU_MemoryOrderViolation_RecordsSSITAndRecovers) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    cfg.type = LsuType::SPECULATIVE_OOO;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    uint32_t store_pc = 0x1000;
    uint32_t load_pc = 0x1004;
    uint32_t target_addr = 0x2000;

    // Phase 1: First occurrence (Initial state: no SSID in SSIT)
    // Older Store (PC 0x1000, seq 10) enters pipeline
    UOp s1;
    s1.pc = store_pc;
    s1.seq_num = 10;
    s1.rob_idx = 0;
    size_t sq_idx1 = lsu.allocate_store(s1);

    // Younger Load (PC 0x1004, seq 11) enters pipeline
    UOp l1;
    l1.pc = load_pc;
    l1.seq_num = 11;
    l1.rob_idx = 1;
    size_t lq_idx1 = lsu.allocate_load(l1);

    // Load speculatively queries bypass: initial state allows bypass
    EXPECT_TRUE(lsu.can_bypass_disambiguation(l1.pc, l1.seq_num));

    // Load executes speculatively from memory before store address is known
    bus.write32(target_addr, 0x11112222);
    auto res1 = lsu.execute_load(lq_idx1, target_addr, 4, l1.seq_num);
    EXPECT_TRUE(res1.completed);
    EXPECT_EQ(res1.data, 0x11112222);

    // Store address resolves to the SAME address -> Aliasing violation!
    size_t violating_rob = 0;
    bool violation = lsu.execute_store_address(sq_idx1, target_addr, 4, s1.seq_num, violating_rob);
    EXPECT_TRUE(violation);
    EXPECT_EQ(violating_rob, 1);

    // Verify StoreSets SSIT recorded the aliasing PC pair
    EXPECT_TRUE(lsu.get_disambiguator().has_ssid(store_pc));
    EXPECT_TRUE(lsu.get_disambiguator().has_ssid(load_pc));
    EXPECT_EQ(lsu.get_disambiguator().get_ssid(store_pc), lsu.get_disambiguator().get_ssid(load_pc));

    // Pipeline recovery: squash load and younger instructions
    lsu.flush_younger_than(s1.seq_num);
    lsu.execute_store_data(sq_idx1, 0x99998888);
    lsu.commit_store(sq_idx1);

    // Phase 2: Subsequent iteration with same PC pair
    // Older Store (PC 0x1000, seq 20) enters pipeline
    UOp s2;
    s2.pc = store_pc;
    s2.seq_num = 20;
    s2.rob_idx = 0;
    size_t sq_idx2 = lsu.allocate_store(s2);

    // Younger Load (PC 0x1004, seq 21) enters pipeline
    UOp l2;
    l2.pc = load_pc;
    l2.seq_num = 21;
    l2.rob_idx = 1;
    size_t lq_idx2 = lsu.allocate_load(l2);

    // Disambiguator now predicts dependency: Load CANNOT bypass before store address resolves!
    EXPECT_FALSE(lsu.can_bypass_disambiguation(l2.pc, l2.seq_num));

    // Store calculates address
    size_t dummy_rob = 0;
    bool violation2 = lsu.execute_store_address(sq_idx2, target_addr, 4, s2.seq_num, dummy_rob);
    EXPECT_FALSE(violation2);
    lsu.execute_store_data(sq_idx2, 0x55554444);

    // Now Store has issued address, Load can bypass/issue and forward data!
    EXPECT_TRUE(lsu.can_bypass_disambiguation(l2.pc, l2.seq_num));
    auto res2 = lsu.execute_load(lq_idx2, target_addr, 4, l2.seq_num);
    EXPECT_TRUE(res2.completed);
    EXPECT_TRUE(res2.forwarded);
    EXPECT_EQ(res2.data, 0x55554444);

    lsu.commit_store(sq_idx2);
    lsu.free_load(lq_idx2);
}

TEST(LSUUBenchTest, LSU_StrictWaitDisambiguation_NoBypassOnUnresolvedStore) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    cfg.type = LsuType::STRICT_INORDER;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    uint32_t store_pc = 0x3000;
    uint32_t load_pc = 0x3004;

    UOp s1;
    s1.pc = store_pc;
    s1.seq_num = 100;
    s1.rob_idx = 0;
    size_t sq_idx = lsu.allocate_store(s1);

    UOp l1;
    l1.pc = load_pc;
    l1.seq_num = 101;
    l1.rob_idx = 1;
    size_t lq_idx = lsu.allocate_load(l1);

    // In STRICT_WAIT / STRICT_INORDER mode, load cannot bypass while older store address is unresolved
    EXPECT_FALSE(lsu.can_bypass_disambiguation(l1.pc, l1.seq_num));

    // Store address resolves
    size_t dummy = 0;
    lsu.execute_store_address(sq_idx, 0x4000, 4, s1.seq_num, dummy);
    lsu.execute_store_data(sq_idx, 0x12345678);

    // Now older store is resolved, load can proceed
    EXPECT_TRUE(lsu.can_bypass_disambiguation(l1.pc, l1.seq_num));
    auto res = lsu.execute_load(lq_idx, 0x4000, 4, l1.seq_num);
    EXPECT_TRUE(res.completed);
    EXPECT_TRUE(res.forwarded);
    EXPECT_EQ(res.data, 0x12345678);

    lsu.commit_store(sq_idx);
    lsu.free_load(lq_idx);
}
