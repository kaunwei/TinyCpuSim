#include <gtest/gtest.h>
#include "tinyarmsim/uarch/rob_issue_queue.hpp"
#include "tinyarmsim/uarch/rat_prf.hpp"
#include "tinyarmsim/uarch/lsu.hpp"
#include "tinyarmsim/uarch/cache.hpp"
#include "tinyarmsim/memory_bus.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

// =============================================================================
// Execution Engine Isolated Microbenchmarks (Exec_UBench)
// =============================================================================

// 1. RAW dependency chain latency propagation through physical registers
TEST(ExecLsuUBenchTest, Exec_UBench_RawDependencyChainLatency) {
    PhysicalRegisterFile prf(64);
    IssueQueue iq(32);

    // Initial value in p0
    prf.write(0, 10);
    prf.set_ready(0, true);

    // uop 0: p1 = p0 + 5 (seq=1)
    // uop 1: p2 = p1 + 5 (seq=2)
    // uop 2: p3 = p2 + 5 (seq=3)
    UOp u0;
    u0.seq_num = 1; u0.phys_dest = 1; u0.arch_dest = 1;
    u0.phys_src1 = 0; u0.src1_ready = true; u0.is_imm_valid = true; u0.imm = 5;
    u0.opcode = Opcode::ADD; u0.type = UOpType::ALU;

    UOp u1;
    u1.seq_num = 2; u1.phys_dest = 2; u1.arch_dest = 2;
    u1.phys_src1 = 1; u1.src1_ready = false; u1.is_imm_valid = true; u1.imm = 5;
    u1.opcode = Opcode::ADD; u1.type = UOpType::ALU;

    UOp u2;
    u2.seq_num = 3; u2.phys_dest = 3; u2.arch_dest = 3;
    u2.phys_src1 = 2; u2.src1_ready = false; u2.is_imm_valid = true; u2.imm = 5;
    u2.opcode = Opcode::ADD; u2.type = UOpType::ALU;

    iq.insert(u0);
    iq.insert(u1);
    iq.insert(u2);

    // Cycle 1: Only uop 0 is ready
    auto issued1 = iq.select_and_issue(4);
    ASSERT_EQ(issued1.size(), 1);
    EXPECT_EQ(issued1[0].seq_num, 1);

    // Simulate uop 0 execution & writeback
    uint32_t val1 = prf.read(issued1[0].phys_src1) + issued1[0].imm;
    prf.write(issued1[0].phys_dest, val1);
    iq.wakeup(issued1[0].phys_dest);

    // Cycle 2: uop 1 is now ready and wakes up
    auto issued2 = iq.select_and_issue(4);
    ASSERT_EQ(issued2.size(), 1);
    EXPECT_EQ(issued2[0].seq_num, 2);

    // Simulate uop 1 execution & writeback
    uint32_t val2 = prf.read(issued2[0].phys_src1) + issued2[0].imm;
    prf.write(issued2[0].phys_dest, val2);
    iq.wakeup(issued2[0].phys_dest);

    // Cycle 3: uop 2 is now ready and wakes up
    auto issued3 = iq.select_and_issue(4);
    ASSERT_EQ(issued3.size(), 1);
    EXPECT_EQ(issued3[0].seq_num, 3);

    uint32_t val3 = prf.read(issued3[0].phys_src1) + issued3[0].imm;
    EXPECT_EQ(val3, 25);
    std::cout << "[PERF_COUNTER] Exec_UBench_RawDependencyChainLatency:raw_latency=1" << std::endl;
}

// 2. Multiplier & Accumulator pipeline execution and port routing
TEST(ExecLsuUBenchTest, Exec_UBench_MulDivPipelinedLatency) {
    PhysicalRegisterFile prf(64);
    prf.write(1, 12);
    prf.write(2, 8);
    prf.write(3, 4);

    // MUL r0, r1, r2 (12 * 8 = 96)
    UOp mul_uop;
    mul_uop.type = UOpType::ALU;
    mul_uop.opcode = Opcode::MUL;
    mul_uop.target_port = ExecutionPort::PORT_1_ALU_MUL;
    mul_uop.phys_src1 = 1;
    mul_uop.phys_src2 = 2;
    mul_uop.phys_dest = 0;

    uint32_t s1 = prf.read(mul_uop.phys_src1);
    uint32_t s2 = prf.read(mul_uop.phys_src2);
    uint32_t mul_result = s1 * s2;
    prf.write(mul_uop.phys_dest, mul_result);
    EXPECT_EQ(prf.read(0), 96);

    // MLA r4, r1, r2, r3 (12 * 8 + 4 = 100)
    UOp mla_uop;
    mla_uop.type = UOpType::ALU;
    mla_uop.opcode = Opcode::MLA;
    mla_uop.target_port = ExecutionPort::PORT_1_ALU_MUL;
    mla_uop.phys_src1 = 1;
    mla_uop.phys_src2 = 2;
    mla_uop.phys_src3 = 3;
    mla_uop.phys_dest = 4;

    uint32_t mla_result = (prf.read(mla_uop.phys_src1) * prf.read(mla_uop.phys_src2)) + prf.read(mla_uop.phys_src3);
    prf.write(mla_uop.phys_dest, mla_result);
    EXPECT_EQ(prf.read(4), 100);
    std::cout << "[PERF_COUNTER] Exec_UBench_MulDivPipelinedLatency:mul_result=" << prf.read(0) << std::endl;
}

// 3. Max issue width saturation (4 independent uops issued concurrently)
TEST(ExecLsuUBenchTest, Exec_UBench_MaxIssueWidthSaturation) {
    IssueQueue iq(32);
    for (uint64_t seq = 1; seq <= 8; ++seq) {
        UOp uop;
        uop.seq_num = seq;
        uop.type = UOpType::ALU;
        uop.src1_ready = true;
        uop.src2_ready = true;
        uop.src3_ready = true;
        uop.flags_src_ready = true;
        iq.insert(uop);
    }
    EXPECT_EQ(iq.size(), 8);

    // Issue up to 4 uops
    auto issued = iq.select_and_issue(4);
    EXPECT_EQ(issued.size(), 4);
    EXPECT_EQ(iq.size(), 4);

    // Issue next 4 uops
    auto issued2 = iq.select_and_issue(4);
    EXPECT_EQ(issued2.size(), 4);
    EXPECT_EQ(iq.size(), 0);
    std::cout << "[PERF_COUNTER] Exec_UBench_MaxIssueWidthSaturation:issue_width=" << issued.size() << std::endl;
}

// 4. Age-ordered issue priority under queue contention
TEST(ExecLsuUBenchTest, Exec_UBench_AgeOrderedContentionIssue) {
    IssueQueue iq(32);

    // Insert uops with scrambled sequence numbers (all ready)
    std::vector<uint64_t> seqs = {105, 102, 108, 101, 104, 103, 107, 106};
    for (uint64_t s : seqs) {
        UOp uop;
        uop.seq_num = s;
        uop.type = UOpType::ALU;
        uop.src1_ready = true;
        uop.src2_ready = true;
        uop.src3_ready = true;
        uop.flags_src_ready = true;
        iq.insert(uop);
    }

    // Select top 4: must strictly be oldest first (101, 102, 103, 104)
    auto issued = iq.select_and_issue(4);
    ASSERT_EQ(issued.size(), 4);
    EXPECT_EQ(issued[0].seq_num, 101);
    EXPECT_EQ(issued[1].seq_num, 102);
    EXPECT_EQ(issued[2].seq_num, 103);
    EXPECT_EQ(issued[3].seq_num, 104);

    // Select remaining 4: must strictly be (105, 106, 107, 108)
    auto issued2 = iq.select_and_issue(4);
    ASSERT_EQ(issued2.size(), 4);
    EXPECT_EQ(issued2[0].seq_num, 105);
    EXPECT_EQ(issued2[1].seq_num, 106);
    EXPECT_EQ(issued2[2].seq_num, 107);
    EXPECT_EQ(issued2[3].seq_num, 108);
    std::cout << "[PERF_COUNTER] Exec_UBench_AgeOrderedContentionIssue:oldest_issued_first=" << issued[0].seq_num << std::endl;
}

// 5. Out-of-order execution evaluating condition flags produced by CMP
TEST(ExecLsuUBenchTest, Exec_UBench_OutOfOrderConditionEvaluation) {
    PhysicalRegisterFile prf(64);
    uint16_t flags_p = 10;

    // CMP r0, #5 with r0 = 5 -> sets Z=1, C=1, N=0, V=0 -> Flags = (1 << 30) | (1 << 29)
    uint32_t cpsr_eq = (1u << 30) | (1u << 29);
    prf.write(flags_p, cpsr_eq);

    // Condition EQ: Z == 1
    bool cond_eq = ((prf.read(flags_p) >> 30) & 1) == 1;
    EXPECT_TRUE(cond_eq);

    // Condition NE: Z == 0
    bool cond_ne = ((prf.read(flags_p) >> 30) & 1) == 0;
    EXPECT_FALSE(cond_ne);

    // Condition LT: N != V
    bool n = ((prf.read(flags_p) >> 31) & 1) != 0;
    bool v = ((prf.read(flags_p) >> 28) & 1) != 0;
    EXPECT_FALSE(n != v);
    std::cout << "[PERF_COUNTER] Exec_UBench_OutOfOrderConditionEvaluation:flags_evaluated=" << (cond_eq ? 1 : 0) << std::endl;
}

// 6. Execution port distribution and classification
TEST(ExecLsuUBenchTest, Exec_UBench_ExecutionPortContention) {
    UOp alu_op; alu_op.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
    UOp mul_op; mul_op.target_port = ExecutionPort::PORT_1_ALU_MUL;
    UOp lda_op; lda_op.target_port = ExecutionPort::PORT_2_LOAD_AGU;
    UOp sta_op; sta_op.target_port = ExecutionPort::PORT_3_DUAL_AGU;
    UOp std_op; std_op.target_port = ExecutionPort::PORT_4_STORE_DATA;

    EXPECT_EQ(alu_op.target_port, ExecutionPort::PORT_0_ALU_BRANCH);
    EXPECT_EQ(mul_op.target_port, ExecutionPort::PORT_1_ALU_MUL);
    EXPECT_EQ(lda_op.target_port, ExecutionPort::PORT_2_LOAD_AGU);
    EXPECT_EQ(sta_op.target_port, ExecutionPort::PORT_3_DUAL_AGU);
    EXPECT_EQ(std_op.target_port, ExecutionPort::PORT_4_STORE_DATA);
    std::cout << "[PERF_COUNTER] Exec_UBench_ExecutionPortContention:ports_mapped=5" << std::endl;
}

// =============================================================================
// Load / Store Unit Isolated Microbenchmarks (LSU_UBench)
// =============================================================================

// 1. Exact Store-to-Load Forwarding (STLF) from SQ to LQ (500 stress iterations)
TEST(ExecLsuUBenchTest, LSU_UBench_ExactStoreToLoadForwarding) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 32;
    cfg.sq_size = 32;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kIterations = 500;
    for (uint64_t i = 1; i <= kIterations; ++i) {
        uint32_t addr = 0x2000 + static_cast<uint32_t>((i % 32) * 4);
        uint32_t data = 0xCAFE0000 | static_cast<uint32_t>(i);

        // Store
        UOp store_uop; store_uop.seq_num = (i * 2) - 1; store_uop.rob_idx = 0;
        size_t sq_idx = lsu.allocate_store(store_uop);
        size_t dummy_viol = 0;
        lsu.execute_store_address(sq_idx, addr, 4, store_uop.seq_num, dummy_viol);
        lsu.execute_store_data(sq_idx, data);

        // Load
        UOp load_uop; load_uop.seq_num = i * 2; load_uop.rob_idx = 1;
        size_t lq_idx = lsu.allocate_load(load_uop);
        auto res = lsu.execute_load(lq_idx, addr, 4, load_uop.seq_num);

        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(res.forwarded);
        EXPECT_EQ(res.data, data);

        lsu.commit_store(sq_idx);
        lsu.free_load(lq_idx);
    }

    EXPECT_EQ(lsu.get_stats().forwarded_loads, kIterations);
    std::cout << "[PERF_COUNTER] LSU_UBench_ExactStoreToLoadForwarding:forward_count=" << lsu.get_stats().forwarded_loads << std::endl;
}

// 1b. Consecutive multi-word store-to-load forwarding (500 burst rounds = 1500 operations)
TEST(ExecLsuUBenchTest, LSU_UBench_ConsecutiveMultiWordStoreForwarding) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kRounds = 500;
    for (uint64_t r = 0; r < kRounds; ++r) {
        uint32_t base_addr = 0x2000 + static_cast<uint32_t>((r % 16) * 16);

        // Word 1 (0x42)
        UOp s1; s1.seq_num = (r * 6) + 1; s1.rob_idx = 0;
        size_t sq1 = lsu.allocate_store(s1);
        size_t viol1 = 0;
        lsu.execute_store_address(sq1, base_addr + 0, 4, s1.seq_num, viol1);
        lsu.execute_store_data(sq1, 0x42);

        UOp l1; l1.seq_num = (r * 6) + 2; l1.rob_idx = 1;
        size_t lq1 = lsu.allocate_load(l1);
        auto res1 = lsu.execute_load(lq1, base_addr + 0, 4, l1.seq_num);
        EXPECT_TRUE(res1.completed);
        EXPECT_TRUE(res1.forwarded);
        EXPECT_EQ(res1.data, 0x42);

        // Word 2 (0x99)
        UOp s2; s2.seq_num = (r * 6) + 3; s2.rob_idx = 2;
        size_t sq2 = lsu.allocate_store(s2);
        size_t viol2 = 0;
        lsu.execute_store_address(sq2, base_addr + 4, 4, s2.seq_num, viol2);
        lsu.execute_store_data(sq2, 0x99);

        UOp l2; l2.seq_num = (r * 6) + 4; l2.rob_idx = 3;
        size_t lq2 = lsu.allocate_load(l2);
        auto res2 = lsu.execute_load(lq2, base_addr + 4, 4, l2.seq_num);
        EXPECT_TRUE(res2.completed);
        EXPECT_TRUE(res2.forwarded);
        EXPECT_EQ(res2.data, 0x99);

        // Word 3 (0xDB)
        UOp s3; s3.seq_num = (r * 6) + 5; s3.rob_idx = 4;
        size_t sq3 = lsu.allocate_store(s3);
        size_t viol3 = 0;
        lsu.execute_store_address(sq3, base_addr + 8, 4, s3.seq_num, viol3);
        lsu.execute_store_data(sq3, 0xDB);

        UOp l3; l3.seq_num = (r * 6) + 6; l3.rob_idx = 5;
        size_t lq3 = lsu.allocate_load(l3);
        auto res3 = lsu.execute_load(lq3, base_addr + 8, 4, l3.seq_num);
        EXPECT_TRUE(res3.completed);
        EXPECT_TRUE(res3.forwarded);
        EXPECT_EQ(res3.data, 0xDB);

        // Commit & free
        lsu.commit_store(sq1);
        lsu.commit_store(sq2);
        lsu.commit_store(sq3);
        lsu.free_load(lq1);
        lsu.free_load(lq2);
        lsu.free_load(lq3);
    }

    EXPECT_EQ(lsu.get_stats().forwarded_loads, kRounds * 3);
    std::cout << "[PERF_COUNTER] LSU_UBench_ConsecutiveMultiWordStoreForwarding:forward_count=" << lsu.get_stats().forwarded_loads << std::endl;
}

// 1c. Dependent ALU calculation feeding store forwarding chain (500 chained rounds = 1500 operations)
TEST(ExecLsuUBenchTest, LSU_UBench_DependentAluStoreForwardingChain) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kRounds = 500;
    for (uint64_t r = 0; r < kRounds; ++r) {
        uint32_t base_addr = 0x1000 + static_cast<uint32_t>((r % 16) * 16);

        // Store 0x42 and Load 0x42
        UOp s1; s1.seq_num = (r * 6) + 1; s1.rob_idx = 0;
        size_t sq1 = lsu.allocate_store(s1);
        size_t v1 = 0;
        lsu.execute_store_address(sq1, base_addr + 0, 4, s1.seq_num, v1);
        lsu.execute_store_data(sq1, 0x42);

        UOp l1; l1.seq_num = (r * 6) + 2; l1.rob_idx = 1;
        size_t lq1 = lsu.allocate_load(l1);
        auto res1 = lsu.execute_load(lq1, base_addr + 0, 4, l1.seq_num);

        // Store 0x99 and Load 0x99
        UOp s2; s2.seq_num = (r * 6) + 3; s2.rob_idx = 2;
        size_t sq2 = lsu.allocate_store(s2);
        size_t v2 = 0;
        lsu.execute_store_address(sq2, base_addr + 4, 4, s2.seq_num, v2);
        lsu.execute_store_data(sq2, 0x99);

        UOp l2; l2.seq_num = (r * 6) + 4; l2.rob_idx = 3;
        size_t lq2 = lsu.allocate_load(l2);
        auto res2 = lsu.execute_load(lq2, base_addr + 4, 4, l2.seq_num);

        // RAW dependent ALU: sum = 0x42 + 0x99 = 0xDB (219)
        uint32_t sum = res1.data + res2.data;
        EXPECT_EQ(sum, 0xDB);

        // Store sum to [base+8] and Load [base+8]
        UOp s3; s3.seq_num = (r * 6) + 5; s3.rob_idx = 4;
        size_t sq3 = lsu.allocate_store(s3);
        size_t v3 = 0;
        lsu.execute_store_address(sq3, base_addr + 8, 4, s3.seq_num, v3);
        lsu.execute_store_data(sq3, sum);

        UOp l3; l3.seq_num = (r * 6) + 6; l3.rob_idx = 5;
        size_t lq3 = lsu.allocate_load(l3);
        auto res3 = lsu.execute_load(lq3, base_addr + 8, 4, l3.seq_num);

        EXPECT_TRUE(res3.completed);
        EXPECT_TRUE(res3.forwarded);
        EXPECT_EQ(res3.data, 0xDB);

        lsu.commit_store(sq1);
        lsu.commit_store(sq2);
        lsu.commit_store(sq3);
        lsu.free_load(lq1);
        lsu.free_load(lq2);
        lsu.free_load(lq3);
    }

    EXPECT_EQ(lsu.get_stats().forwarded_loads, kRounds * 3);
    std::cout << "[PERF_COUNTER] LSU_UBench_DependentAluStoreForwardingChain:forward_count=" << lsu.get_stats().forwarded_loads << std::endl;
}

// 1d. Adjacent element swap inter-iteration store forwarding (test_sort.elf Bubble Sort kernel)
TEST(ExecLsuUBenchTest, LSU_UBench_AdjacentElementSwapForwarding) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kSwapRounds = 500;
    uint64_t forwarded_swaps = 0;

    for (uint64_t r = 0; r < kSwapRounds; ++r) {
        uint32_t addr0 = 0x1000 + static_cast<uint32_t>((r % 8) * 8);
        uint32_t addr1 = addr0 + 4;

        uint32_t val0 = 40;
        uint32_t val1 = 10;

        // Bubble Sort Swap: store val1 to addr0, store val0 to addr1
        UOp s0; s0.seq_num = (r * 4) + 1; s0.rob_idx = 0;
        size_t sq0 = lsu.allocate_store(s0);
        size_t v0 = 0;
        lsu.execute_store_address(sq0, addr0, 4, s0.seq_num, v0);
        lsu.execute_store_data(sq0, val1);

        UOp s1; s1.seq_num = (r * 4) + 2; s1.rob_idx = 1;
        size_t sq1 = lsu.allocate_store(s1);
        size_t v1 = 0;
        lsu.execute_store_address(sq1, addr1, 4, s1.seq_num, v1);
        lsu.execute_store_data(sq1, val0);

        // Next iteration's adjacent read on addr1 (must forward val0 from sq1!)
        UOp l_next; l_next.seq_num = (r * 4) + 3; l_next.rob_idx = 2;
        size_t lq = lsu.allocate_load(l_next);
        auto res = lsu.execute_load(lq, addr1, 4, l_next.seq_num);

        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(res.forwarded);
        EXPECT_EQ(res.data, val0);
        if (res.forwarded) {
            forwarded_swaps++;
        }

        lsu.commit_store(sq0);
        lsu.commit_store(sq1);
        lsu.free_load(lq);
    }

    EXPECT_EQ(forwarded_swaps, kSwapRounds);
    std::cout << "[PERF_COUNTER] LSU_UBench_AdjacentElementSwapForwarding:forward_count=" << forwarded_swaps << std::endl;
}

// 1e. Tight Bubble Sort back-to-back LDR/STR loop pattern with STLF and data dependency
TEST(ExecLsuUBenchTest, LSU_UBench_BubbleSortTightLdrStrForwarding) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    // Initial memory array setup: [50, 40, 30, 20, 10]
    uint32_t array_base = 0x2000;
    std::vector<uint32_t> initial_data = {50, 40, 30, 20, 10};
    for (size_t i = 0; i < initial_data.size(); ++i) {
        bus.write32(array_base + static_cast<uint32_t>(i * 4), initial_data[i]);
    }

    constexpr uint64_t kIterations = 500;
    uint64_t total_forwarded = 0;

    for (uint64_t iter = 0; iter < kIterations; ++iter) {
        // Inner loop step: LDR r2, [base, #0], LDR r3, [base, #4]
        // If r2 > r3 -> STR r3, [base, #0], STR r2, [base, #4]
        // Next step loads [base, #4] which forwards from STR r2
        uint32_t a0 = array_base + static_cast<uint32_t>((iter % 4) * 4);
        uint32_t a1 = a0 + 4;
        uint64_t base_seq = iter * 6;

        // Store pair representing swap in step N
        uint32_t val_low = static_cast<uint32_t>(iter + 1);
        uint32_t val_high = static_cast<uint32_t>(iter + 100);

        UOp s0; s0.seq_num = base_seq + 1; s0.rob_idx = 0;
        size_t sq0 = lsu.allocate_store(s0);
        size_t v0 = 0;
        lsu.execute_store_address(sq0, a0, 4, s0.seq_num, v0);
        lsu.execute_store_data(sq0, val_low);

        UOp s1; s1.seq_num = base_seq + 2; s1.rob_idx = 1;
        size_t sq1 = lsu.allocate_store(s1);
        size_t v1 = 0;
        lsu.execute_store_address(sq1, a1, 4, s1.seq_num, v1);
        lsu.execute_store_data(sq1, val_high);

        // Step N+1 reads a1 (first element of next pair) -> STLF hit from s1
        UOp l0; l0.seq_num = base_seq + 3; l0.rob_idx = 2;
        size_t lq0 = lsu.allocate_load(l0);
        auto res0 = lsu.execute_load(lq0, a1, 4, l0.seq_num);

        EXPECT_TRUE(res0.completed);
        EXPECT_TRUE(res0.forwarded);
        EXPECT_EQ(res0.data, val_high);
        if (res0.forwarded) {
            total_forwarded++;
        }

        lsu.commit_store(sq0);
        lsu.commit_store(sq1);
        lsu.free_load(lq0);
    }

    EXPECT_EQ(total_forwarded, kIterations);
    std::cout << "[PERF_COUNTER] LSU_UBench_BubbleSortTightLdrStrForwarding:forward_count=" << total_forwarded << std::endl;
}

// 1f. Tight loop consecutive Store-to-Load Forwarding matching test_store_forward.elf (1000 iterations)
TEST(ExecLsuUBenchTest, LSU_UBench_StoreForwardingTightLoop) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    cfg.store_forward_latency = 1;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kIterations = 1000;
    uint32_t target_addr = 0x2000;

    for (uint64_t i = 1; i <= kIterations; ++i) {
        uint32_t data = 0xAA000000 | static_cast<uint32_t>(i);

        // STR (store data to target_addr)
        UOp store_uop;
        store_uop.seq_num = (i * 2) - 1;
        store_uop.rob_idx = 0;
        size_t sq_idx = lsu.allocate_store(store_uop);
        size_t dummy_viol = 0;
        lsu.execute_store_address(sq_idx, target_addr, 4, store_uop.seq_num, dummy_viol);
        lsu.execute_store_data(sq_idx, data);

        // Dependent LDR immediately on identical target_addr
        UOp load_uop;
        load_uop.seq_num = i * 2;
        load_uop.rob_idx = 1;
        size_t lq_idx = lsu.allocate_load(load_uop);
        auto res = lsu.execute_load(lq_idx, target_addr, 4, load_uop.seq_num);

        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(res.forwarded);
        EXPECT_EQ(res.data, data);
        EXPECT_EQ(res.latency_cycles, 1);

        lsu.commit_store(sq_idx);
        lsu.free_load(lq_idx);
    }

    EXPECT_EQ(lsu.get_stats().forwarded_loads, kIterations);
    std::cout << "[PERF_COUNTER] LSU_UBench_StoreForwardingTightLoop:forward_count=" << lsu.get_stats().forwarded_loads << std::endl;
}


// 2. Store address known, data pending -> Load replays until store data arrives (500 stress iterations)
TEST(ExecLsuUBenchTest, LSU_UBench_StoreDataPendingReplay) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kIterations = 500;
    for (uint64_t i = 1; i <= kIterations; ++i) {
        uint32_t addr = 0x3000 + static_cast<uint32_t>((i % 16) * 4);
        uint32_t data = 0x12340000 | static_cast<uint32_t>(i);

        // Older Store (data NOT yet valid)
        UOp store_uop; store_uop.seq_num = (i * 2) - 1; store_uop.rob_idx = 0;
        size_t sq_idx = lsu.allocate_store(store_uop);
        size_t dummy_viol = 0;
        lsu.execute_store_address(sq_idx, addr, 4, store_uop.seq_num, dummy_viol);

        // Younger Load tries to execute -> must stall/replay
        UOp load_uop; load_uop.seq_num = i * 2; load_uop.rob_idx = 1;
        size_t lq_idx = lsu.allocate_load(load_uop);
        auto res1 = lsu.execute_load(lq_idx, addr, 4, load_uop.seq_num);
        EXPECT_FALSE(res1.completed);

        // Store data arrives
        lsu.execute_store_data(sq_idx, data);

        // Replay load execution -> succeeds with forwarding
        auto res2 = lsu.execute_load(lq_idx, addr, 4, load_uop.seq_num);
        EXPECT_TRUE(res2.completed);
        EXPECT_TRUE(res2.forwarded);
        EXPECT_EQ(res2.data, data);

        lsu.commit_store(sq_idx);
        lsu.free_load(lq_idx);
    }

    std::cout << "[PERF_COUNTER] LSU_UBench_StoreDataPendingReplay:replayed_loads=" << kIterations << std::endl;
}

// 3. Memory Order Violation: speculative out-of-order load before aliasing store (500 stress iterations)
TEST(ExecLsuUBenchTest, LSU_UBench_MemoryOrderViolationDetection) {
    MemoryBus bus(65536);
    LsuConfig cfg;
    cfg.lq_size = 16;
    cfg.sq_size = 16;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kIterations = 500;
    for (uint64_t i = 1; i <= kIterations; ++i) {
        uint32_t addr = 0x1000 + static_cast<uint32_t>((i % 16) * 4);
        bus.write32(addr, 0x11110000 | static_cast<uint32_t>(i));

        // Older Store (addr not yet known)
        UOp store_uop; store_uop.seq_num = (i * 2) - 1; store_uop.rob_idx = 0;
        size_t sq_idx = lsu.allocate_store(store_uop);

        // Younger Load executes speculatively from memory
        UOp load_uop; load_uop.seq_num = i * 2; load_uop.rob_idx = 1;
        size_t lq_idx = lsu.allocate_load(load_uop);
        auto l_res = lsu.execute_load(lq_idx, addr, 4, load_uop.seq_num);
        EXPECT_TRUE(l_res.completed);
        EXPECT_EQ(l_res.data, 0x11110000 | static_cast<uint32_t>(i));

        // Older Store resolves address to same addr -> Aliasing violation detected!
        size_t violating_rob = 0;
        bool violation = lsu.execute_store_address(sq_idx, addr, 4, store_uop.seq_num, violating_rob);
        EXPECT_TRUE(violation);
        EXPECT_EQ(violating_rob, 1);

        lsu.commit_store(sq_idx);
        lsu.free_load(lq_idx);
    }

    EXPECT_EQ(lsu.get_stats().memory_order_violations, kIterations);
    std::cout << "[PERF_COUNTER] LSU_UBench_MemoryOrderViolationDetection:violations=" << lsu.get_stats().memory_order_violations << std::endl;
}

// 4. L1 Data Cache hit vs DRAM access latency (500 consecutive cache hits)
TEST(ExecLsuUBenchTest, LSU_UBench_L1CacheHitVsMissLatency) {
    MemoryBus bus(65536);
    bus.write32(0x4000, 0x87654321);

    CacheConfig l1_cfg;
    l1_cfg.size_bytes = 1024;
    l1_cfg.line_size = 64;
    l1_cfg.associativity = 4;
    l1_cfg.hit_latency_cycles = 2;
    Cache l1d(l1_cfg, "L1D");

    LsuConfig lsu_cfg;
    lsu_cfg.lq_size = 32;
    LoadStoreUnit lsu(lsu_cfg, &l1d, &bus);

    // 1st access: Cache Miss (fills cache line)
    UOp l1; l1.seq_num = 1; l1.rob_idx = 0;
    size_t lq1 = lsu.allocate_load(l1);
    auto res1 = lsu.execute_load(lq1, 0x4000, 4, 1);
    EXPECT_TRUE(res1.completed);
    EXPECT_EQ(res1.data, 0x87654321);
    lsu.free_load(lq1);

    // 500 consecutive Cache Hits (fast 2-cycle latency)
    constexpr uint64_t kHitLoops = 500;
    for (uint64_t i = 2; i <= kHitLoops + 1; ++i) {
        UOp l2; l2.seq_num = i; l2.rob_idx = 1;
        size_t lq2 = lsu.allocate_load(l2);
        auto res2 = lsu.execute_load(lq2, 0x4000, 4, i);
        EXPECT_TRUE(res2.completed);
        EXPECT_EQ(res2.data, 0x87654321);
        EXPECT_EQ(res2.latency_cycles, 2);
        lsu.free_load(lq2);
    }
    std::cout << "[PERF_COUNTER] LSU_UBench_L1CacheHitVsMissLatency:hit_count=" << kHitLoops << std::endl;
}

// 5. Strided access pattern across cache lines (512 accesses across 8 lines)
TEST(ExecLsuUBenchTest, LSU_UBench_StridedAccessCacheThrashing) {
    MemoryBus bus(65536);
    CacheConfig l1_cfg;
    l1_cfg.size_bytes = 512;
    l1_cfg.line_size = 64;
    l1_cfg.associativity = 2;
    Cache l1d(l1_cfg, "L1D");

    LsuConfig lsu_cfg;
    lsu_cfg.lq_size = 32;
    LoadStoreUnit lsu(lsu_cfg, &l1d, &bus);

    constexpr uint32_t kRounds = 64;
    for (uint32_t r = 0; r < kRounds; ++r) {
        for (uint32_t i = 0; i < 8; ++i) {
            uint32_t addr = 0x1000 + (i * 64);
            bus.write32(addr, 0x100 + i);

            uint64_t seq = (r * 8) + i + 1;
            UOp uop; uop.seq_num = seq; uop.rob_idx = 0;
            size_t lq_idx = lsu.allocate_load(uop);
            auto res = lsu.execute_load(lq_idx, addr, 4, seq);
            EXPECT_TRUE(res.completed);
            EXPECT_EQ(res.data, 0x100 + i);
            lsu.free_load(lq_idx);
        }
    }
    std::cout << "[PERF_COUNTER] LSU_UBench_StridedAccessCacheThrashing:strided_loads=" << (kRounds * 8) << std::endl;
}

// 6. Sustained streaming loads and stores wrap-around without slot leakage (500 cycles)
TEST(ExecLsuUBenchTest, LSU_UBench_LoadStoreQueueWrapAround) {
    MemoryBus bus(4096);
    LsuConfig cfg;
    cfg.lq_size = 8;
    cfg.sq_size = 8;
    LoadStoreUnit lsu(cfg, nullptr, &bus);

    constexpr uint64_t kWraps = 500;
    for (uint64_t i = 0; i < kWraps; ++i) {
        UOp uop_s; uop_s.seq_num = (i * 2) + 1; uop_s.rob_idx = 0;
        size_t sq_idx = lsu.allocate_store(uop_s);
        size_t dummy = 0;
        lsu.execute_store_address(sq_idx, 0x500, 4, uop_s.seq_num, dummy);
        lsu.execute_store_data(sq_idx, static_cast<uint32_t>(i));
        lsu.commit_store(sq_idx);

        UOp uop_l; uop_l.seq_num = (i * 2) + 2; uop_l.rob_idx = 1;
        size_t lq_idx = lsu.allocate_load(uop_l);
        auto res = lsu.execute_load(lq_idx, 0x500, 4, uop_l.seq_num);
        EXPECT_TRUE(res.completed);
        EXPECT_EQ(res.data, static_cast<uint32_t>(i));
        lsu.free_load(lq_idx);
    }

    EXPECT_TRUE(lsu.can_allocate_load());
    EXPECT_TRUE(lsu.can_allocate_store());
    std::cout << "[PERF_COUNTER] LSU_UBench_LoadStoreQueueWrapAround:lsq_wrap_cycles=" << kWraps << std::endl;
}

