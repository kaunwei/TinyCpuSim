#include <gtest/gtest.h>
#include <vector>
#include <iostream>
#include "tinyarmsim/uarch/lsd.hpp"
#include "tinyarmsim/uarch/uop.hpp"

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
