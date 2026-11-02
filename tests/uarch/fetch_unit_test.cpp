#include <gtest/gtest.h>
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/uarch/fetch_unit.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

TEST(FetchUnitTest, FetchesMixedThumb16AndThumb32Instructions) {
    MemoryBus bus(64 * 1024); // 64KB
    
    // Address 0x1000: MOV R0, #42 (Thumb-16: 0x202A)
    bus.write16(0x1000, 0x202A);
    // Address 0x1002: MOVT R0, #0x1234 (Thumb-32: 0xF2C1 0x2034)
    bus.write16(0x1002, 0xF2C1);
    bus.write16(0x1004, 0x2034);
    // Address 0x1006: ADD R1, R0, #1 (Thumb-16: 0x3101)
    bus.write16(0x1006, 0x3101);

    CoreConfig core_cfg;
    core_cfg.fetch_width = 4;
    BranchPredictorConfig bp_cfg;
    bp_cfg.type = PredictorType::NONE;

    FetchUnit fetch(0x1000, bus, nullptr, core_cfg, bp_cfg);

    fetch.tick();

    EXPECT_TRUE(fetch.has_uops());
    EXPECT_GE(fetch.queue_size(), 3);

    // Pop 1st: MOV R0, #42
    UOp uop1 = fetch.pop_uop();
    EXPECT_EQ(uop1.pc, 0x1000);
    EXPECT_EQ(uop1.type, UOpType::ALU);

    // Pop 2nd: MOVT
    UOp uop2 = fetch.pop_uop();
    EXPECT_EQ(uop2.pc, 0x1002);
    EXPECT_EQ(uop2.type, UOpType::ALU);

    // Pop 3rd: ADD R1, R0, #1
    UOp uop3 = fetch.pop_uop();
    EXPECT_EQ(uop3.pc, 0x1006);
    EXPECT_EQ(uop3.type, UOpType::ALU);
}

TEST(FetchUnitTest, PipelineFlushClearsQueueAndRedirectsPc) {
    MemoryBus bus(64 * 1024);
    bus.write16(0x1000, 0x202A); // MOV R0, #42
    bus.write16(0x1002, 0x3101); // ADD R1, R0, #1

    CoreConfig core_cfg;
    core_cfg.fetch_width = 2;
    BranchPredictorConfig bp_cfg;

    FetchUnit fetch(0x1000, bus, nullptr, core_cfg, bp_cfg);

    fetch.tick();
    EXPECT_TRUE(fetch.has_uops());

    // Flush front-end to 0x2000
    fetch.flush(0x2000);
    EXPECT_FALSE(fetch.has_uops());
    EXPECT_EQ(fetch.get_pc(), 0x2000);
}

TEST(FetchUnitTest, BranchPredictorRedirectsFetchPc) {
    MemoryBus bus(64 * 1024);
    // B target (+16 bytes -> 0x1014) (Thumb-16: 0xE006)
    bus.write16(0x1000, 0xE006);
    // At target 0x1010: NOP (0xBF00)
    bus.write16(0x1010, 0xBF00);

    CoreConfig core_cfg;
    core_cfg.fetch_width = 2;
    BranchPredictorConfig bp_cfg;
    bp_cfg.type = PredictorType::BIMODAL;
    bp_cfg.btb_size = 512;

    FetchUnit fetch(0x1000, bus, nullptr, core_cfg, bp_cfg);

    fetch.tick();
    EXPECT_TRUE(fetch.has_uops());
    UOp branch_uop = fetch.pop_uop();
    EXPECT_EQ(branch_uop.pc, 0x1000);
    EXPECT_TRUE(branch_uop.pred_taken);

    // PC should have redirected
    EXPECT_NE(fetch.get_pc(), 0x1002);
}
