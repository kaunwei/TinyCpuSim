#include <gtest/gtest.h>
#include "platforms/01_basic_perf/topology.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

TEST(BasicPerfPlatformTest, ExecuteFibonacciWorkload) {
    BasicPerfPlatform platform;
    platform.load_workload(PERF_WORKLOAD_ELF);
    uint64_t steps = platform.run(50000);
    EXPECT_GT(steps, 0u);
}
