#include "tinyarmsim/time/scheduler.hpp"
#include <gtest/gtest.h>

TEST(StandaloneTimeLib, EventPriorityQueueOrdering) {
    using namespace tinyarmsim::time;
    TimeScheduler scheduler;
    std::vector<uint64_t> timestamps;

    scheduler.schedule(5, EventPhase::Device, [&] { timestamps.push_back(5); });
    scheduler.schedule(1, EventPhase::Device, [&] { timestamps.push_back(1); });
    scheduler.schedule(3, EventPhase::Device, [&] { timestamps.push_back(3); });
    scheduler.schedule(10, EventPhase::Device, [&] { timestamps.push_back(10); });

    // Also verify FIFO ordering for same timestamp and phase
    std::vector<int> fifo_order;
    scheduler.schedule(2, EventPhase::Device, [&] { fifo_order.push_back(100); });
    scheduler.schedule(2, EventPhase::Device, [&] { fifo_order.push_back(200); });
    scheduler.schedule(2, EventPhase::Device, [&] { fifo_order.push_back(300); });

    const auto result = scheduler.run(10, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(result.progress, 10u);
    EXPECT_EQ(result.reason, StopReason::Deadline);
    EXPECT_EQ(scheduler.now(), 10u);

    const std::vector<uint64_t> expected_timestamps = {1, 3, 5, 10};
    EXPECT_EQ(timestamps, expected_timestamps);

    const std::vector<int> expected_fifo = {100, 200, 300};
    EXPECT_EQ(fifo_order, expected_fifo);
}

TEST(StandaloneTimeLib, TwoPhaseDispatchOrderingAndBoundary) {
    using namespace tinyarmsim::time;
    TimeScheduler scheduler;
    std::vector<int> sequence;

    // Schedule Irq before Device to ensure phase precedence over insertion order
    scheduler.schedule(1, EventPhase::Irq, [&] { sequence.push_back(20); });
    scheduler.schedule(1, EventPhase::Device, [&] { sequence.push_back(10); });
    scheduler.schedule(1, EventPhase::Irq, [&] { sequence.push_back(21); });
    scheduler.schedule(1, EventPhase::Device, [&] { sequence.push_back(11); });

    // Step should interleave between time ticks
    const auto result = scheduler.run(2, 1, [&] {
        sequence.push_back(0);
        return StepResult::Progress;
    });

    EXPECT_EQ(result.progress, 2u);
    EXPECT_EQ(result.reason, StopReason::Deadline);
    // At t=0: step runs (sequence: 0).
    // At t=1: Device events (10, 11), then Irq events (20, 21), then step runs (0).
    const std::vector<int> expected_sequence = {0, 10, 11, 20, 21, 0};
    EXPECT_EQ(sequence, expected_sequence);

    // Verify boundary enforcement: cannot schedule an earlier phase at current time
    TimeScheduler phase_boundary_scheduler;
    bool caught_exception = false;
    phase_boundary_scheduler.schedule(1, EventPhase::Irq, [&] {
        try {
            phase_boundary_scheduler.schedule(1, EventPhase::Device, [] {});
        } catch (const std::invalid_argument&) {
            caught_exception = true;
        }
    });
    phase_boundary_scheduler.run(2, 1, [] { return StepResult::Progress; });
    EXPECT_TRUE(caught_exception);
}

TEST(StandaloneTimeLib, RemainderRetentionAcrossRunsAndEarlyStops) {
    using namespace tinyarmsim::time;
    TimeScheduler scheduler;
    std::vector<uint64_t> executed;

    scheduler.schedule(2, EventPhase::Device, [&] { executed.push_back(2); });
    scheduler.schedule(5, EventPhase::Device, [&] { executed.push_back(5); });
    scheduler.schedule(8, EventPhase::Device, [&] { executed.push_back(8); });

    // Run first to deadline 3: only t=2 event should fire; t=5 and t=8 remain
    auto result1 = scheduler.run(3, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(result1.progress, 3u);
    EXPECT_EQ(result1.reason, StopReason::Deadline);
    EXPECT_EQ(scheduler.now(), 3u);
    EXPECT_EQ(executed, (std::vector<uint64_t>{2}));

    // Next run to deadline 6: t=5 fires; t=8 remains
    auto result2 = scheduler.run(6, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(result2.progress, 3u);
    EXPECT_EQ(result2.reason, StopReason::Deadline);
    EXPECT_EQ(scheduler.now(), 6u);
    EXPECT_EQ(executed, (std::vector<uint64_t>{2, 5}));

    // Next run to deadline 10: t=8 fires
    auto result3 = scheduler.run(10, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(result3.progress, 4u);
    EXPECT_EQ(result3.reason, StopReason::Deadline);
    EXPECT_EQ(scheduler.now(), 10u);
    EXPECT_EQ(executed, (std::vector<uint64_t>{2, 5, 8}));

    // Verify remainder retention on early Halt stop
    TimeScheduler halt_scheduler;
    std::vector<uint64_t> halt_executed;
    halt_scheduler.schedule(1, EventPhase::Device, [&] { halt_executed.push_back(1); });
    halt_scheduler.schedule(4, EventPhase::Device, [&] { halt_executed.push_back(4); });

    uint64_t step_count = 0;
    auto halt_result = halt_scheduler.run(10, 1, [&] {
        ++step_count;
        return step_count == 2 ? StepResult::Halt : StepResult::Progress;
    });
    EXPECT_EQ(halt_result.reason, StopReason::Halt);
    EXPECT_EQ(halt_result.progress, 1u);
    EXPECT_EQ(halt_scheduler.now(), 1u);
    EXPECT_EQ(halt_executed, (std::vector<uint64_t>{1}));

    // Subsequent run retains event at t=4
    auto resume_result = halt_scheduler.run(5, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(resume_result.reason, StopReason::Deadline);
    EXPECT_EQ(halt_executed, (std::vector<uint64_t>{1, 4}));
}

TEST(StandaloneTimeLib, EventCancellation) {
    using namespace tinyarmsim::time;
    TimeScheduler scheduler;
    bool event1_ran = false;
    bool event2_ran = false;
    bool event3_ran = false;

    auto handle1 = scheduler.schedule(2, EventPhase::Device, [&] { event1_ran = true; });
    auto handle2 = scheduler.schedule(2, EventPhase::Device, [&] { event2_ran = true; });
    scheduler.schedule(3, EventPhase::Device, [&] { event3_ran = true; });

    // Cancel handle1 and test idempotence by cancelling multiple times
    handle1.cancel();
    handle1.cancel();

    // Default-constructed handle cancellation is safe no-op
    Handle empty_handle;
    empty_handle.cancel();

    const auto result = scheduler.run(4, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(result.progress, 4u);
    EXPECT_EQ(result.reason, StopReason::Deadline);
    EXPECT_FALSE(event1_ran);
    EXPECT_TRUE(event2_ran);
    EXPECT_TRUE(event3_ran);

    // Cancelled event does not wake up scheduler in Wait mode
    TimeScheduler wait_scheduler;
    bool wait_event_ran = false;
    auto wait_handle = wait_scheduler.schedule(5, EventPhase::Device, [&] { wait_event_ran = true; });
    wait_handle.cancel();

    const auto wait_result = wait_scheduler.run(10, 1, [] { return StepResult::Wait; });
    EXPECT_EQ(wait_result.reason, StopReason::Wait);
    EXPECT_EQ(wait_scheduler.now(), 0u);
    EXPECT_FALSE(wait_event_ran);
}
