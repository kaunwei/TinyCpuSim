#include <gtest/gtest.h>
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/memory_hierarchy.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

TEST(ComposableCacheTest, DedicatedL1AndDedicatedL2) {
    MemoryBus bus(64 * 1024 * 1024);
    UArchConfig cfg{};
    cfg.num_cores = 1;
    cfg.dram_latency_cycles = 80;
    cfg.coherence = CoherenceProtocol::NONE;
    cfg.l2_shared.type = CacheType::PASSTHROUGH; // Shared L2 disabled

    CoreConfig core_cfg{};
    core_cfg.l1d.type = CacheType::SET_ASSOCIATIVE;
    core_cfg.l1d.hit_latency_cycles = 1;
    core_cfg.l1d.size_bytes = 4096;
    core_cfg.l1d.line_size = 64;
    core_cfg.l1d.associativity = 2;

    CacheConfig priv_l2{};
    priv_l2.type = CacheType::SET_ASSOCIATIVE;
    priv_l2.hit_latency_cycles = 10;
    priv_l2.size_bytes = 16384;
    priv_l2.line_size = 64;
    priv_l2.associativity = 4;
    core_cfg.private_l2 = priv_l2;

    cfg.default_core = core_cfg;

    CoherentMemoryHierarchy mem(bus, cfg);
    EXPECT_NE(mem.get_private_l2(0), nullptr);

    // Initial access: cold miss in L1, cold miss in L2 -> L1(1) + L2(10) + DRAM(80) = 91
    auto resp = mem.read_data(0, 0x1000, 4);
    EXPECT_FALSE(resp.is_l1_hit);
    EXPECT_EQ(resp.latency_cycles, 91u);

    // Second access to same line: L1 hit -> 1 cycle
    resp = mem.read_data(0, 0x1004, 4);
    EXPECT_TRUE(resp.is_l1_hit);
    EXPECT_EQ(resp.latency_cycles, 1u);
}

TEST(ComposableCacheTest, DedicatedL1DirectToBus) {
    MemoryBus bus(64 * 1024 * 1024);
    UArchConfig cfg{};
    cfg.num_cores = 1;
    cfg.dram_latency_cycles = 80;
    cfg.coherence = CoherenceProtocol::NONE;
    cfg.l2_shared.type = CacheType::PASSTHROUGH;

    CoreConfig core_cfg{};
    core_cfg.l1d.type = CacheType::SET_ASSOCIATIVE;
    core_cfg.l1d.hit_latency_cycles = 2;
    core_cfg.l1d.size_bytes = 4096;
    core_cfg.l1d.line_size = 64;
    core_cfg.l1d.associativity = 2;
    core_cfg.private_l2 = std::nullopt; // Direct to bus

    cfg.default_core = core_cfg;

    CoherentMemoryHierarchy mem(bus, cfg);
    EXPECT_EQ(mem.get_private_l2(0), nullptr);

    // Cold miss in L1 -> L1 hit (2) + DRAM (80) = 82
    auto resp = mem.read_data(0, 0x2000, 4);
    EXPECT_FALSE(resp.is_l1_hit);
    EXPECT_TRUE(resp.is_dram_access);
    EXPECT_EQ(resp.latency_cycles, 82u);
}

TEST(ComposableCacheTest, MultiCoreSharedL2Coherence) {
    MemoryBus bus(64 * 1024 * 1024);
    UArchConfig cfg = UArchConfig::make_multicore_default(2);
    cfg.dram_latency_cycles = 80;
    cfg.coherence = CoherenceProtocol::MESI;

    CoherentMemoryHierarchy mem(bus, cfg);

    // Core 0 reads 0x3000
    auto resp0 = mem.read_data(0, 0x3000, 4);
    EXPECT_FALSE(resp0.is_l1_hit);

    // Core 1 reads 0x3000
    auto resp1 = mem.read_data(1, 0x3000, 4);
    EXPECT_FALSE(resp1.is_l1_hit);

    // Core 0 writes 0x3000 -> Invalidation
    mem.write_data(0, 0x3000, 0x9999, 4);

    // Core 1 reads 0x3000 again -> gets updated data
    auto resp1_after = mem.read_data(1, 0x3000, 4);
    EXPECT_EQ(resp1_after.data, 0x9999u);
}
