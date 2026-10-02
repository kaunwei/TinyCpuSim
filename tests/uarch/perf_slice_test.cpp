#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include "tinyarmsim/uarch/slice_manager.hpp"

using namespace tinyarmsim::uarch;

TEST(PerfSliceTest, SliceConfigDefaultAndActiveCheck) {
    SliceConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_FALSE(cfg.is_active());

    cfg.enabled = true;
    cfg.interval_instructions = 1000;
    EXPECT_TRUE(cfg.is_active());

    cfg.interval_instructions = 0;
    cfg.interval_ticks = 500;
    EXPECT_TRUE(cfg.is_active());
}

TEST(PerfSliceTest, SliceDeltaAndCumulativeCalculation) {
    SliceManager manager;
    SliceConfig cfg;
    cfg.enabled = true;
    cfg.interval_instructions = 500;
    cfg.format = SliceFormat::Text;
    manager.set_config(cfg);

    // Initial state: tick 0, inst 0
    EXPECT_FALSE(manager.should_trigger_instruction(400));
    EXPECT_TRUE(manager.should_trigger_instruction(500));

    // Snapshot 1 at cycle 1000, inst 500
    UArchStats stats1;
    stats1.total_simulated_cycles = 1000;
    CoreStats c1;
    c1.cycles = 1000;
    c1.committed_instructions = 500;
    c1.committed_uops = 750;
    c1.branch.record_prediction(true);
    c1.branch.record_prediction(false);
    c1.l1d.record_access(true);
    stats1.cores.push_back(c1);

    const auto& s1 = manager.capture_slice(stats1);
    EXPECT_EQ(s1.slice_id, 0);
    EXPECT_EQ(s1.start_cycle, 0);
    EXPECT_EQ(s1.end_cycle, 1000);
    EXPECT_EQ(s1.start_instruction, 0);
    EXPECT_EQ(s1.end_instruction, 500);
    EXPECT_EQ(s1.slice_cycles(), 1000);
    EXPECT_EQ(s1.slice_instructions(), 500);
    EXPECT_DOUBLE_EQ(s1.slice_ipc(), 0.5);

    EXPECT_EQ(s1.stats_delta.total_simulated_cycles, 1000);
    EXPECT_EQ(s1.stats_delta.total_committed_instructions(), 500);
    EXPECT_EQ(s1.stats_delta.cores[0].branch.predictions, 2);
    EXPECT_EQ(s1.stats_delta.cores[0].branch.mispredictions, 1);

    // After snapshot 1, next threshold should be relative to 500 insts
    EXPECT_FALSE(manager.should_trigger_instruction(900));
    EXPECT_TRUE(manager.should_trigger_instruction(1000));

    // Snapshot 2 at cycle 2500, inst 1100
    UArchStats stats2;
    stats2.total_simulated_cycles = 2500;
    CoreStats c2;
    c2.cycles = 2500;
    c2.committed_instructions = 1100;
    c2.committed_uops = 1600;
    c2.branch.predictions = 5;
    c2.branch.correct_predictions = 3;
    c2.branch.mispredictions = 2;
    c2.l1d.accesses = 10;
    c2.l1d.hits = 8;
    c2.l1d.misses = 2;
    stats2.cores.push_back(c2);

    const auto& s2 = manager.capture_slice(stats2);
    EXPECT_EQ(s2.slice_id, 1);
    EXPECT_EQ(s2.start_cycle, 1000);
    EXPECT_EQ(s2.end_cycle, 2500);
    EXPECT_EQ(s2.start_instruction, 500);
    EXPECT_EQ(s2.end_instruction, 1100);
    EXPECT_EQ(s2.slice_cycles(), 1500);
    EXPECT_EQ(s2.slice_instructions(), 600);
    EXPECT_DOUBLE_EQ(s2.slice_ipc(), 600.0 / 1500.0);

    // Verify delta isolation
    EXPECT_EQ(s2.stats_delta.total_simulated_cycles, 1500);
    EXPECT_EQ(s2.stats_delta.total_committed_instructions(), 600);
    EXPECT_EQ(s2.stats_delta.cores[0].branch.predictions, 3); // 5 - 2 = 3
    EXPECT_EQ(s2.stats_delta.cores[0].branch.mispredictions, 1); // 2 - 1 = 1
    EXPECT_EQ(s2.stats_delta.cores[0].l1d.accesses, 9); // 10 - 1 = 9
    EXPECT_EQ(s2.stats_delta.cores[0].l1d.hits, 7); // 8 - 1 = 7

    EXPECT_EQ(manager.num_slices(), 2);
}

TEST(PerfSliceTest, MultiFormatExportGem5JsonAndText) {
    SliceConfig cfg;
    cfg.enabled = true;
    cfg.interval_instructions = 100;

    SliceManager manager(cfg);

    UArchStats stats;
    stats.total_simulated_cycles = 500;
    CoreStats c0;
    c0.cycles = 500;
    c0.committed_instructions = 250;
    c0.committed_uops = 350;
    c0.branch.predictions = 40;
    c0.branch.mispredictions = 4;
    c0.l1i.accesses = 200;
    c0.l1i.hits = 195;
    c0.l1i.misses = 5;
    c0.l1d.accesses = 100;
    c0.l1d.hits = 90;
    c0.l1d.misses = 10;
    c0.lsu.loads = 60;
    c0.lsu.stores = 40;
    stats.cores.push_back(c0);

    const auto& slice = manager.capture_slice(stats);

    // Text format
    std::string txt = slice.format(SliceFormat::Text);
    EXPECT_NE(txt.find("TinyCpuSim Performance Slice #0"), std::string::npos);
    EXPECT_NE(txt.find("Interval Cycles:           [0 -> 500]"), std::string::npos);
    EXPECT_NE(txt.find("Interval IPC:              0.500"), std::string::npos);

    // Gem5 format
    std::string gem5_txt = slice.format(SliceFormat::Gem5);
    EXPECT_NE(gem5_txt.find("---------- Begin Simulation Statistics ----------"), std::string::npos);
    EXPECT_NE(gem5_txt.find("simInsts"), std::string::npos);
    EXPECT_NE(gem5_txt.find("system.cpu_cluster.cpus.numCycles"), std::string::npos);
    EXPECT_NE(gem5_txt.find("system.cpu_cluster.cpus.ipc"), std::string::npos);
    EXPECT_NE(gem5_txt.find("---------- End Simulation Statistics   ----------"), std::string::npos);

    // JSON format
    std::string json_txt = slice.format(SliceFormat::JSON);
    EXPECT_NE(json_txt.find("\"slice_id\": 0"), std::string::npos);
    EXPECT_NE(json_txt.find("\"slice_ipc\": 0.5"), std::string::npos);
    EXPECT_NE(json_txt.find("\"delta\":"), std::string::npos);
    EXPECT_NE(json_txt.find("\"cumulative\":"), std::string::npos);
}

TEST(PerfSliceTest, OutputFileStreamingAppendsSlices) {
    std::string temp_file = "/tmp/tinyarmsim_test_slices.txt";
    std::filesystem::remove(temp_file);

    SliceConfig cfg;
    cfg.enabled = true;
    cfg.interval_ticks = 100;
    cfg.output_file = temp_file;
    cfg.format = SliceFormat::Gem5;

    SliceManager manager(cfg);

    UArchStats stats1;
    stats1.total_simulated_cycles = 100;
    CoreStats c1;
    c1.cycles = 100;
    c1.committed_instructions = 50;
    stats1.cores.push_back(c1);
    manager.capture_slice(stats1);

    UArchStats stats2;
    stats2.total_simulated_cycles = 200;
    CoreStats c2;
    c2.cycles = 200;
    c2.committed_instructions = 120;
    stats2.cores.push_back(c2);
    manager.capture_slice(stats2);

    std::ifstream ifs(temp_file);
    ASSERT_TRUE(ifs.is_open());
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    // Verify both slices exist in the stream
    size_t count = 0;
    std::string marker = "---------- Begin Simulation Statistics ----------";
    size_t pos = 0;
    while ((pos = content.find(marker, pos)) != std::string::npos) {
        count++;
        pos += marker.length();
    }
    EXPECT_EQ(count, 2);

    std::filesystem::remove(temp_file);
}
