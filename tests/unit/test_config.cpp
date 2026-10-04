#include <gtest/gtest.h>
#include <sstream>
#include "tinyarmsim/uarch/config.hpp"

using namespace tinyarmsim::uarch;

TEST(ConfigTest, PrefetcherDefaultNone) {
    CacheConfig cache_cfg;
    EXPECT_EQ(cache_cfg.prefetcher, PrefetcherType::NONE);
    EXPECT_EQ(cache_cfg.prefetch_distance, 1);
    EXPECT_EQ(cache_cfg.prefetch_queue_size, 8);
    EXPECT_FALSE(cache_cfg.is_prefetch_enabled());
    EXPECT_NO_THROW(cache_cfg.validate());
}

TEST(ConfigTest, PrefetcherEnumAndValues) {
    CacheConfig cfg;
    cfg.prefetcher = PrefetcherType::NEXT_LINE;
    cfg.prefetch_distance = 2;
    cfg.prefetch_queue_size = 16;
    EXPECT_TRUE(cfg.is_prefetch_enabled());
    EXPECT_NO_THROW(cfg.validate());

    cfg.prefetcher = PrefetcherType::STRIDE;
    cfg.prefetch_distance = 4;
    cfg.prefetch_queue_size = 32;
    EXPECT_TRUE(cfg.is_prefetch_enabled());
    EXPECT_NO_THROW(cfg.validate());

    cfg.prefetcher = PrefetcherType::STREAM;
    cfg.prefetch_distance = 8;
    cfg.prefetch_queue_size = 64;
    EXPECT_TRUE(cfg.is_prefetch_enabled());
    EXPECT_NO_THROW(cfg.validate());
}

TEST(ConfigTest, PrefetcherValidationRejectsZeroDistanceOrQueue) {
    CacheConfig cfg;
    cfg.prefetcher = PrefetcherType::STRIDE;
    cfg.prefetch_distance = 0;
    cfg.prefetch_queue_size = 8;
    EXPECT_THROW(cfg.validate(), std::invalid_argument);

    cfg.prefetch_distance = 4;
    cfg.prefetch_queue_size = 0;
    EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ConfigTest, ParseKvPrefetcherInCacheSections) {
    std::string config_content = R"(
[global]
num_cores = 1

[l1d]
type = SET_ASSOCIATIVE
size_bytes = 32768
associativity = 4
prefetcher = STRIDE
prefetch_distance = 4
prefetch_queue_size = 16

[l2]
type = SET_ASSOCIATIVE
size_bytes = 524288
associativity = 8
prefetcher = NEXT_LINE
prefetch_distance = 2
prefetch_queue_size = 32

[l1i]
type = SET_ASSOCIATIVE
size_bytes = 32768
associativity = 4
prefetcher = STREAM
prefetch_distance = 6
prefetch_queue_size = 24
)";

    std::istringstream iss(config_content);
    UArchConfig cfg = UArchConfig::parse_kv(iss);

    EXPECT_EQ(cfg.default_core.l1d.prefetcher, PrefetcherType::STRIDE);
    EXPECT_EQ(cfg.default_core.l1d.prefetch_distance, 4);
    EXPECT_EQ(cfg.default_core.l1d.prefetch_queue_size, 16);
    EXPECT_TRUE(cfg.default_core.l1d.is_prefetch_enabled());

    EXPECT_EQ(cfg.l2_shared.prefetcher, PrefetcherType::NEXT_LINE);
    EXPECT_EQ(cfg.l2_shared.prefetch_distance, 2);
    EXPECT_EQ(cfg.l2_shared.prefetch_queue_size, 32);
    EXPECT_TRUE(cfg.l2_shared.is_prefetch_enabled());

    EXPECT_EQ(cfg.default_core.l1i.prefetcher, PrefetcherType::STREAM);
    EXPECT_EQ(cfg.default_core.l1i.prefetch_distance, 6);
    EXPECT_EQ(cfg.default_core.l1i.prefetch_queue_size, 24);
    EXPECT_TRUE(cfg.default_core.l1i.is_prefetch_enabled());
}

TEST(ConfigTest, ParseKvPrefetcherDedicatedSection) {
    std::string config_content = R"(
[global]
num_cores = 1

[prefetcher]
type = STRIDE
distance = 3
queue_size = 12
)";

    std::istringstream iss(config_content);
    UArchConfig cfg = UArchConfig::parse_kv(iss);

    EXPECT_EQ(cfg.default_core.l1d.prefetcher, PrefetcherType::STRIDE);
    EXPECT_EQ(cfg.default_core.l1d.prefetch_distance, 3);
    EXPECT_EQ(cfg.default_core.l1d.prefetch_queue_size, 12);
}

TEST(ConfigTest, BackwardCompatibilityWithExistingConfigs) {
    std::string config_content = R"(
[global]
num_cores = 1
enable_mesi = true
dram_latency = 80

[core]
enable_ooo = true
fetch_width = 4
decode_width = 4
issue_width = 4
commit_width = 4
rob_size = 64
rs_size = 32
num_phys_regs = 128

[branch_predictor]
enabled = true
type = TAGE
table_size = 4096

[lsu]
enabled = true
lq_size = 16
sq_size = 16

[l1i]
enabled = true
size_bytes = 32768
line_size = 64
associativity = 4
hit_latency = 1

[l1d]
enabled = true
size_bytes = 32768
line_size = 64
associativity = 4
hit_latency = 1

[l2]
enabled = true
size_bytes = 524288
line_size = 64
associativity = 8
hit_latency = 10
)";

    std::istringstream iss(config_content);
    UArchConfig cfg = UArchConfig::parse_kv(iss);

    EXPECT_EQ(cfg.default_core.l1i.prefetcher, PrefetcherType::NONE);
    EXPECT_EQ(cfg.default_core.l1d.prefetcher, PrefetcherType::NONE);
    EXPECT_EQ(cfg.l2_shared.prefetcher, PrefetcherType::NONE);
    EXPECT_FALSE(cfg.default_core.l1d.is_prefetch_enabled());
}
