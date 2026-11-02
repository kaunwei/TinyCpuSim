#include <gtest/gtest.h>
#include "tinyarmsim/uarch/topdown_profiler.hpp"
#include "tinyarmsim/cpu/interpreter.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

TEST(RoiM5OpsTest, ResetAndDumpStatsIsolatesRoiExecution) {
    TopDownProfiler profiler;
    profiler.init(1, 2);

    // Initial phase before ROI (Warmup / Init): 20 slots
    for (int i = 0; i < 20; ++i) {
        profiler.record_slot(0, SlotType::RetiringBaseAlu);
    }
    EXPECT_EQ(profiler.get_report(0).total_slots, 20);

    // Trigger m5_reset_stats (0x50)
    profiler.trigger_m5op(0x50);
    EXPECT_EQ(profiler.get_report(0).total_slots, 0);
    EXPECT_TRUE(profiler.is_roi_active());

    // ROI Phase (Algorithm Execution): 100 slots
    for (int i = 0; i < 80; ++i) {
        profiler.record_slot(0, SlotType::RetiringBaseAlu);
    }
    for (int i = 0; i < 20; ++i) {
        profiler.record_slot(0, SlotType::BackEndMemL1DMiss);
    }
    EXPECT_EQ(profiler.get_report(0).total_slots, 100);

    // Trigger m5_dump_stats (0x51) -> Freezes snapshot
    profiler.trigger_m5op(0x51);
    EXPECT_FALSE(profiler.is_roi_active());

    // Post-ROI Phase (Teardown): additional slots should be ignored by snapshot
    for (int i = 0; i < 30; ++i) {
        profiler.record_slot(0, SlotType::RetiringBaseAlu);
    }

    // Report strictly contains only the 100 ROI slots!
    auto rep = profiler.get_report(0);
    EXPECT_EQ(rep.total_slots, 100);
    EXPECT_EQ(rep.retiring_slots, 80);
    EXPECT_EQ(rep.backend_slots, 20);
    EXPECT_DOUBLE_EQ(rep.retiring_pct(), 80.0);
    EXPECT_DOUBLE_EQ(rep.backend_pct(), 20.0);
}
