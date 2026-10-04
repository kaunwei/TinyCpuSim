#include <gtest/gtest.h>
#include <vector>
#include <iostream>
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/uarch/lsu.hpp"
#include "tinyarmsim/uarch/memory_disambiguator.hpp"

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

