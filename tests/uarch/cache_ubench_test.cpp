#include <gtest/gtest.h>
#include "tinyarmsim/uarch/cache.hpp"
#include "tinyarmsim/uarch/coherence.hpp"
#include "tinyarmsim/uarch/memory_hierarchy.hpp"
#include "tinyarmsim/uarch/prefetcher.hpp"
#include "tinyarmsim/memory_bus.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

// =============================================================================
// Cache & Memory Hierarchy Isolated Microbenchmarks (Cache_UBench)
// =============================================================================

// 1. L1 cache hit latency, tag matching, and statistics tracking
TEST(CacheUBenchTest, Cache_UBench_L1HitLatencyAndThroughput) {
    CacheConfig cfg;
    cfg.size_bytes = 4096;
    cfg.line_size = 64;
    cfg.associativity = 4;
    cfg.hit_latency_cycles = 2;
    Cache l1(cfg, "L1_Test");

    uint32_t addr = 0x1000;

    // First access: Cache Miss
    auto res1 = l1.access(addr, false, 1);
    EXPECT_FALSE(res1.hit);
    EXPECT_EQ(l1.get_stats().misses, 1);
    EXPECT_EQ(l1.get_stats().hits, 0);

    // Second access to same address: Cache Hit with exact 2-cycle latency
    auto res2 = l1.access(addr, false, 2);
    EXPECT_TRUE(res2.hit);
    EXPECT_EQ(res2.latency_cycles, 2);
    EXPECT_EQ(l1.get_stats().hits, 1);

    // Third access to offset within same 64-byte line (0x1020): Cache Hit
    auto res3 = l1.access(addr + 0x20, false, 3);
    EXPECT_TRUE(res3.hit);
    EXPECT_EQ(res3.latency_cycles, 2);
    EXPECT_EQ(l1.get_stats().hits, 2);
    EXPECT_DOUBLE_EQ(l1.get_stats().hit_rate(), 2.0 / 3.0);
    std::cout << "[PERF_COUNTER] Cache_UBench_L1HitLatencyAndThroughput:hit_latency=" << res2.latency_cycles << std::endl;
}

// 2. N-way Set Associativity and LRU replacement algorithm precision
TEST(CacheUBenchTest, Cache_UBench_LruReplacementSetAssociativity) {
    CacheConfig cfg;
    cfg.size_bytes = 256;      // 256 bytes total
    cfg.line_size = 64;        // 4 lines total
    cfg.associativity = 2;     // 2 sets, 2 ways per set
    cfg.replacement = ReplacementPolicy::LRU;
    Cache cache(cfg, "LRU_Test");

    EXPECT_EQ(cache.get_num_sets(), 2);

    // Set 0 addresses (offset 0..63, index bit = 0):
    // addr0: tag 0, set 0 -> 0x0000
    // addr1: tag 1, set 0 -> 0x0080
    // addr2: tag 2, set 0 -> 0x0100
    uint32_t addr0 = 0x0000;
    uint32_t addr1 = 0x0080;
    uint32_t addr2 = 0x0100;

    // Fill Set 0 (Way 0: addr0, Way 1: addr1)
    cache.access(addr0, false, 1);
    cache.access(addr1, false, 2);

    EXPECT_TRUE(cache.probe(addr0));
    EXPECT_TRUE(cache.probe(addr1));

    // Access addr0 again to make addr1 the LRU line
    cache.access(addr0, false, 3);

    // Insert addr2 into Set 0 -> addr1 (LRU) must be evicted!
    auto res = cache.access(addr2, false, 4);
    EXPECT_FALSE(res.hit);
    EXPECT_TRUE(res.evicted);
    EXPECT_EQ(res.evicted_addr, addr1);

    // Now addr0 and addr2 are present, addr1 is gone
    EXPECT_TRUE(cache.probe(addr0));
    EXPECT_TRUE(cache.probe(addr2));
    EXPECT_FALSE(cache.probe(addr1));
    std::cout << "[PERF_COUNTER] Cache_UBench_LruReplacementSetAssociativity:evicted_addr=" << res.evicted_addr << std::endl;
}

// 3. Write-back dirty line eviction and writeback counter tracking
TEST(CacheUBenchTest, Cache_UBench_WriteBackDirtyEviction) {
    CacheConfig cfg;
    cfg.size_bytes = 128;      // 128 bytes
    cfg.line_size = 64;        // 2 lines total
    cfg.associativity = 1;     // Direct mapped: 2 sets (Set 0 and Set 1)
    cfg.write_policy = WritePolicy::WRITE_BACK;
    Cache cache(cfg, "WriteBack_Test");

    uint32_t addr_a = 0x0000; // Set 0
    uint32_t addr_b = 0x0080; // Set 0 (collides with addr_a)

    // Store to addr_a -> marks line dirty
    cache.access(addr_a, true, 1);
    EXPECT_EQ(cache.get_stats().writebacks, 0);

    // Access addr_b (collides in Set 0) -> Evicts addr_a which is dirty!
    auto res = cache.access(addr_b, false, 2);
    EXPECT_FALSE(res.hit);
    EXPECT_TRUE(res.evicted);
    EXPECT_TRUE(res.evicted_dirty);
    EXPECT_EQ(res.evicted_addr, addr_a);
    EXPECT_EQ(cache.get_stats().writebacks, 1);
    std::cout << "[PERF_COUNTER] Cache_UBench_WriteBackDirtyEviction:writebacks=" << cache.get_stats().writebacks << std::endl;
}

// 4. Non-blocking MSHR (Miss Status Holding Register) capacity allocation
TEST(CacheUBenchTest, Cache_UBench_MshrNonBlockingAllocation) {
    CacheConfig cfg;
    cfg.size_bytes = 1024;
    cfg.line_size = 64;
    cfg.associativity = 2;
    cfg.mshr_entries = 4; // Max 4 outstanding non-blocking misses
    Cache cache(cfg, "MSHR_Test");

    // Generate 4 independent line misses
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t addr = 0x1000 + (i * 64);
        auto res = cache.access(addr, false, 10);
        EXPECT_FALSE(res.hit);
        EXPECT_TRUE(res.mshr_allocated);
    }

    // 5th independent miss exceeds MSHR capacity
    uint32_t addr5 = 0x2000;
    auto res5 = cache.access(addr5, false, 10);
    EXPECT_FALSE(res5.hit);
    EXPECT_FALSE(res5.mshr_allocated); // MSHR is full!
    std::cout << "[PERF_COUNTER] Cache_UBench_MshrNonBlockingAllocation:mshr_concurrency=4" << std::endl;
}

// 5. Full MESI coherence state machine transitions on multi-core snooping
TEST(CacheUBenchTest, Cache_UBench_MesiCoherenceStateTransitions) {
    MESICoherenceEngine engine(2, 64); // 2 cores, 64B cache line
    uint32_t line_addr = 0x5000;

    // Initial state: Core 0 and Core 1 have INVALID copy
    EXPECT_EQ(engine.get_state(0, line_addr), MESIState::INVALID);
    EXPECT_EQ(engine.get_state(1, line_addr), MESIState::INVALID);

    // 1. Core 0 reads -> No peer has copy -> Transitions to EXCLUSIVE
    auto act0 = engine.handle_cpu_read(0, line_addr);
    EXPECT_EQ(act0.new_state, MESIState::EXCLUSIVE);
    EXPECT_EQ(act0.bus_tx, BusTransactionType::BUS_RD);

    // 2. Core 1 reads same line -> Peer has copy -> Both become SHARED
    auto act1 = engine.handle_cpu_read(1, line_addr);
    EXPECT_EQ(act1.new_state, MESIState::SHARED);
    EXPECT_EQ(engine.get_state(0, line_addr), MESIState::SHARED);
    EXPECT_EQ(engine.get_state(1, line_addr), MESIState::SHARED);

    // 3. Core 0 writes -> Upgrades to MODIFIED, invalidates Core 1
    auto act_wr = engine.handle_cpu_write(0, line_addr);
    EXPECT_EQ(act_wr.new_state, MESIState::MODIFIED);
    EXPECT_EQ(act_wr.bus_tx, BusTransactionType::BUS_UPGR);
    EXPECT_EQ(engine.get_state(0, line_addr), MESIState::MODIFIED);
    EXPECT_EQ(engine.get_state(1, line_addr), MESIState::INVALID);

    // 4. Core 1 reads -> Core 0 must flush dirty data to bus, both become SHARED
    auto act_rd_peer = engine.handle_cpu_read(1, line_addr);
    EXPECT_EQ(act_rd_peer.new_state, MESIState::SHARED);
    EXPECT_EQ(engine.get_state(0, line_addr), MESIState::SHARED);
    std::cout << "[PERF_COUNTER] Cache_UBench_MesiCoherenceStateTransitions:mesi_transitions=4" << std::endl;
}

// 6. Memory hierarchy inclusion and latency progression (L1 Miss -> L2 Hit -> DRAM Miss)
TEST(CacheUBenchTest, Cache_UBench_SharedL2HierarchicalInclusion) {
    MemoryBus bus(65536);
    bus.write32(0x1000, 0x12345678);

    UArchConfig cfg;
    cfg.num_cores = 1;
    cfg.dram_latency_cycles = 50;

    cfg.default_core.l1d.size_bytes = 512;
    cfg.default_core.l1d.line_size = 64;
    cfg.default_core.l1d.hit_latency_cycles = 2;

    cfg.l2_shared.size_bytes = 4096;
    cfg.l2_shared.line_size = 64;
    cfg.l2_shared.hit_latency_cycles = 10;

    CoherentMemoryHierarchy mem_hier(bus, cfg);

    // Access 1: Cold start -> L1 Miss, L2 Miss -> DRAM Access (50+ cycles)
    auto resp1 = mem_hier.read_data(0, 0x1000, 4, 1);
    EXPECT_EQ(resp1.data, 0x12345678);
    EXPECT_TRUE(resp1.is_dram_access);
    EXPECT_GE(resp1.latency_cycles, 50);

    // Access 2: L1 Hit (2 cycles)
    auto resp2 = mem_hier.read_data(0, 0x1000, 4, 2);
    EXPECT_EQ(resp2.data, 0x12345678);
    EXPECT_TRUE(resp2.is_l1_hit);
    EXPECT_EQ(resp2.latency_cycles, 2);
    std::cout << "[PERF_COUNTER] Cache_UBench_SharedL2HierarchicalInclusion:l2_inclusion_latency=" << resp2.latency_cycles << std::endl;
}

// 7. Stride Prefetcher State Machine Transitions (Initial -> Transient -> Steady)
TEST(CacheUBenchTest, StridePrefetcherStateMachine) {
    // Test PC-indexed stride state transitions with delta=+64 and delta=-64
    StridePrefetcher prefetcher(16, 2); // 16 entries, degree 2
    
    uint32_t pc = 0x80001000;
    
    // Initial access: state is Initial / untracked
    EXPECT_EQ(prefetcher.get_state(pc), StrideState::INITIAL);
    auto pfs0 = prefetcher.access(pc, 0x1000);
    EXPECT_TRUE(pfs0.empty());
    EXPECT_EQ(prefetcher.get_state(pc), StrideState::INITIAL);
    
    // Second access with delta = +64: transitions to TRANSIENT
    auto pfs1 = prefetcher.access(pc, 0x1040);
    EXPECT_TRUE(pfs1.empty());
    EXPECT_EQ(prefetcher.get_state(pc), StrideState::TRANSIENT);
    EXPECT_EQ(prefetcher.get_stride(pc), 64);
    
    // Third access with same delta (+64): transitions to STEADY and emits prefetches
    auto pfs2 = prefetcher.access(pc, 0x1080);
    EXPECT_EQ(prefetcher.get_state(pc), StrideState::STEADY);
    EXPECT_EQ(prefetcher.get_stride(pc), 64);
    ASSERT_EQ(pfs2.size(), 2);
    EXPECT_EQ(pfs2[0], 0x10C0); // 0x1080 + 64
    EXPECT_EQ(pfs2[1], 0x1100); // 0x1080 + 128
    
    // Test negative stride delta = -64 with different PC
    uint32_t pc_neg = 0x80002000;
    EXPECT_EQ(prefetcher.get_state(pc_neg), StrideState::INITIAL);
    auto pfn0 = prefetcher.access(pc_neg, 0x5000);
    EXPECT_TRUE(pfn0.empty());
    EXPECT_EQ(prefetcher.get_state(pc_neg), StrideState::INITIAL);
    
    auto pfn1 = prefetcher.access(pc_neg, 0x4FC0); // -64
    EXPECT_TRUE(pfn1.empty());
    EXPECT_EQ(prefetcher.get_state(pc_neg), StrideState::TRANSIENT);
    EXPECT_EQ(prefetcher.get_stride(pc_neg), -64);
    
    auto pfn2 = prefetcher.access(pc_neg, 0x4F80); // -64
    EXPECT_EQ(prefetcher.get_state(pc_neg), StrideState::STEADY);
    EXPECT_EQ(prefetcher.get_stride(pc_neg), -64);
    ASSERT_EQ(pfn2.size(), 2);
    EXPECT_EQ(pfn2[0], 0x4F40); // 0x4F80 - 64
    EXPECT_EQ(pfn2[1], 0x4F00); // 0x4F80 - 128
    
    // Also test NextLinePrefetcher basic functionality
    NextLinePrefetcher next_line_pf(64, 2);
    auto nlpfs = next_line_pf.access(0x1000);
    ASSERT_EQ(nlpfs.size(), 2);
    EXPECT_EQ(nlpfs[0], 0x1040);
    EXPECT_EQ(nlpfs[1], 0x1080);
}

