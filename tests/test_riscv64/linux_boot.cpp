// Linux 启动过程中实际踩到的坑，逐条钉成回归测试。
// 这些行为单看规范都不算"新特性"，但只要有一处退化，内核就会在启动早期挂死，
// 而且现象往往是"卡在某个自旋里"，从日志上看不出是 CPU 的问题。
#include <gtest/gtest.h>

#include <cstring>

#include "platform64.h"

namespace {

// ---- 指令编码：platform64.h 没覆盖的部分 ----
constexpr uint32_t ECALL = 0x00000073u;
constexpr uint32_t MRET = 0x30200073u;
constexpr uint32_t SRET = 0x10200073u;

// BRANCH：imm[12|10:5] 在 inst[31:25]，imm[4:1|11] 在 inst[11:7]
constexpr uint32_t enc_b(int f3, int rs1, int rs2, int32_t off) {
    const uint32_t u = (uint32_t) off;
    return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25)
         | ((uint32_t)(rs2 & 0x1F) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(f3 & 7) << 12) | (((u >> 1) & 0xFu) << 8)
         | (((u >> 11) & 1u) << 7) | 0x63u;
}
constexpr uint32_t add(int rd, int rs1, int rs2) { return enc_r(0x33, rd, 0, rs1, rs2, 0); }
constexpr uint32_t or_(int rd, int rs1, int rs2) { return enc_r(0x33, rd, 6, rs1, rs2, 0); }
constexpr uint32_t lw(int rd, int rs1, int32_t imm) { return enc_i(0x03, rd, 2, rs1, imm); }
constexpr uint32_t lb(int rd, int rs1, int32_t imm) { return enc_i(0x03, rd, 0, rs1, imm); }
constexpr uint32_t sb(int rs2, int rs1, int32_t imm) { return enc_s(0, rs1, rs2, imm); }
constexpr uint32_t sw(int rs2, int rs1, int32_t imm) { return enc_s(2, rs1, rs2, imm); }
// lr.w/sc.w 的 funct5 编码在 imm[11:5]，rs2 在 imm[4:0]
constexpr uint32_t lr_w(int rd, int rs1) {
    return (0x02u << 27) | ((uint32_t) rs1 << 15) | (2u << 12) | ((uint32_t) rd << 7) | 0x2Fu;
}
constexpr uint32_t sc_w(int rd, int rs1, int rs2) {
    return (0x03u << 27) | ((uint32_t) rs2 << 20) | ((uint32_t) rs1 << 15)
         | (2u << 12) | ((uint32_t) rd << 7) | 0x2Fu;
}
// 读 CSR：csrrs rd, csr, x0
constexpr uint32_t csr_rd(int rd, int csr) { return csrrs(rd, csr, 0); }

// ---- CSR 编号 ----
constexpr int CSR_SSTATUS = 0x100, CSR_SIE = 0x104, CSR_STVEC = 0x105;
constexpr int CSR_SCOUNTEREN = 0x106, CSR_SSCRATCH = 0x140, CSR_SEPC = 0x141;
constexpr int CSR_SCAUSE = 0x142, CSR_SIP = 0x144, CSR_SATP = 0x180;
constexpr int CSR_MSTATUS = 0x300, CSR_MISA = 0x301, CSR_MEDELEG = 0x302;
constexpr int CSR_MIDELEG = 0x303, CSR_MTVEC = 0x305, CSR_MCOUNTEREN = 0x306;
constexpr int CSR_MSCRATCH = 0x340, CSR_MEPC = 0x341, CSR_MCAUSE = 0x342;
constexpr int CSR_TIME = 0xC01;

// 特权级数值与 CPU 内部一致：0=U 1=S 3=M
constexpr uint8_t PRIV_U = 0, PRIV_S = 1, PRIV_M = 3;

// 扩展号：SBI 的 a7
constexpr uint32_t SBI_EXT_BASE = 0x10;
constexpr uint32_t SBI_EXT_TIME = 0x54494D45; // "TIME"

struct LinuxFixture : Rv64Fixture {
    void word(uint32_t addr, uint32_t v) {
        uint8_t *m = mem->writableMemPtr() + addr;
        m[0] = (uint8_t) (v & 0xFF);
        m[1] = (uint8_t) ((v >> 8) & 0xFF);
        m[2] = (uint8_t) ((v >> 16) & 0xFF);
        m[3] = (uint8_t) ((v >> 24) & 0xFF);
    }
    void words(uint32_t addr, std::initializer_list<uint32_t> list) {
        uint32_t a = addr;
        for (uint32_t v : list) {
            word(a, v);
            a += 4;
        }
    }
    uint32_t mem32(uint32_t addr) const {
        const uint8_t *m = mem->writableMemPtr() + addr;
        return (uint32_t) m[0] | ((uint32_t) m[1] << 8) | ((uint32_t) m[2] << 16)
             | ((uint32_t) m[3] << 24);
    }
    // 从 M 态跳进 S 态的引导代码，返回占用的字数。
    // mstatus.MPP=S 必须靠 lui+addi 拼：0x800 作为 12 位立即数会被符号扩展成负数
    uint32_t enterS(uint32_t entry) {
        words(0x00, {
            addi(1, 0, (int32_t) entry),
            csrrw(0, CSR_MEPC, 1),
            lui(1, 0x1),          // x1 = 0x1000
            addi(1, 1, -2048),    // x1 = 0x800 → mstatus.MPP=S
            csrrw(0, CSR_MSTATUS, 1),
            MRET,
        });
        return 6;
    }
    // 构造 64 位常量：lui 给高 20 位，addi 补低 12 位（有符号）。
    // hi 要先按 +0x800 进位再取，否则低 12 位为负时 lui 的值差一
    static void loadImm(int rd, uint64_t v, std::vector<uint32_t> &out) {
        const uint32_t hi = (uint32_t) ((v + 0x800) >> 12);
        const int32_t lo = (int32_t) (v - ((uint64_t) hi << 12));
        if (hi == 0) {
            out.push_back(addi(rd, 0, (int32_t) v));
            return;
        }
        out.push_back(lui(rd, hi));
        out.push_back(addi(rd, rd, lo));
    }
};

// ============================================================
// 坑 1：S 态 ecall 被当成陷阱处理，PC 不前进 → SBI 调用死循环
// ============================================================
TEST(LinuxBoot, SEcallAdvancesPc) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(10, 0, 5),     // a0 = 5
        ECALL,              // legacy set_timer：处理完必须回到下一条
        addi(5, 0, 0x42),
        sw(5, 0, 0x100),    // mem[0x100] = 0x42
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0x42u)
        << "S 态 ecall 没有前进到下一条指令（SBI 调用会无限重复 ecall）";
}

// ============================================================
// 坑 2：sbiret 的返回值放错寄存器（放 a0 而不是 a1），内核把所有 SBI 调用判为失败
// ============================================================
TEST(LinuxBoot, SbiBaseSpecVersionInA1) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(17, 0, SBI_EXT_BASE), // a7 = BASE
        addi(16, 0, 0),            // a6 = get_spec_version
        ECALL,
        sw(10, 0, 0x100),          // a0 是 error
        sw(11, 0, 0x104),          // a1 才是 value
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0u) << "成功时 a0 必须是 0";
    EXPECT_EQ(f.mem32(0x104), 0x01000000u)
        << "spec version 必须是 1.0（0x01000000），报 0.1 会让内核切到 legacy 约定";
}

TEST(LinuxBoot, SbiBaseProbeExtensionTakesArgInA0) {
    LinuxFixture f;
    std::vector<uint32_t> s;
    s.push_back(addi(17, 0, SBI_EXT_BASE));
    s.push_back(addi(16, 0, 3));       // a6 = probe_extension
    f.loadImm(10, SBI_EXT_TIME, s);    // 参数在 a0
    s.push_back(ECALL);
    s.push_back(sw(10, 0, 0x100));
    s.push_back(sw(11, 0, 0x104));
    s.push_back(sw(5, 0, 0x108));
    s.push_back(spin());
    f.enterS(0x40);
    uint32_t a = 0x40;
    for (uint32_t v : s) {
        f.word(a, v);
        a += 4;
    }
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0u);
    EXPECT_EQ(f.mem32(0x104), 1u) << "TIME 扩展必须报告为可用";
}

TEST(LinuxBoot, SbiBaseProbeUnknownExtensionReturnsZero) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(17, 0, SBI_EXT_BASE),
        addi(16, 0, 3),
        addi(10, 0, 0x1234), // 未实现的扩展号
        ECALL,
        sw(11, 0, 0x104),
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x104), 0u) << "没实现的扩展必须报 0，否则内核会去调用它";
}

TEST(LinuxBoot, SbiUnknownExtensionReturnsNotSupported) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(17, 0, 0x99), // 不存在的扩展
        ECALL,
        sw(10, 0, 0x100),
        spin(),
    });
    f.run(400);
    EXPECT_EQ((uint64_t) f.cpu->reg(10), (uint64_t) -2)
        << "未知扩展必须返回 SBI_ERR_NOT_SUPPORTED(-2)，且同样走 a0";
}

// BASE 的实现 ID / 版本：回 0 是合法的，但不能落到 default 的 -2，
// 否则内核日志里会打出一串 0xffff...fdf4
TEST(LinuxBoot, SbiBaseImplIdAndVersionSucceed) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(17, 0, SBI_EXT_BASE),
        addi(16, 0, 1),        // get_impl_id
        ECALL,
        sw(10, 0, 0x100),
        addi(16, 0, 2),        // get_impl_version
        ECALL,
        sw(10, 0, 0x104),
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0u) << "get_impl_id 必须返回成功而不是 -2";
    EXPECT_EQ(f.mem32(0x104), 0u) << "get_impl_version 必须返回成功而不是 -2";
}

// probe_extension 必须把 SRST 也报成可用：内核关机优先走 1.0 的 SRST，
// 探测不到就退到 legacy shutdown，行为不一样
TEST(LinuxBoot, SbiBaseProbeSrst) {
    LinuxFixture f;
    std::vector<uint32_t> s;
    s.push_back(addi(17, 0, SBI_EXT_BASE));
    s.push_back(addi(16, 0, 3));
    f.loadImm(10, 0x53525354ull, s); // "SRST"
    s.push_back(ECALL);
    s.push_back(sw(11, 0, 0x104));
    s.push_back(spin());
    f.enterS(0x40);
    uint32_t a = 0x40;
    for (uint32_t v : s) {
        f.word(a, v);
        a += 4;
    }
    f.run(400);
    EXPECT_EQ(f.mem32(0x104), 1u);
}

// legacy set_timer（a7=0，没有 a6 功能号）同样要成功返回到下一条。
// 内核早期和不少测试代码仍用它
TEST(LinuxBoot, SbiLegacySetTimer) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(17, 0, 0),        // a7 = legacy SET_TIMER
        addi(10, 0, 1000),     // deadline
        ECALL,
        sw(10, 0, 0x100),
        addi(5, 0, 0x42),
        sw(5, 0, 0x104),
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0u);
    EXPECT_EQ(f.mem32(0x104), 0x42u) << "legacy set_timer 之后没有前进";
}

// legacy shutdown（a7=8）：旧内核/测试程序的关机路径
TEST(LinuxBoot, SbiLegacyShutdownHalts) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(17, 0, 8),
        ECALL,
        addi(5, 0, 0x42),
        sw(5, 0, 0x100),
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0u) << "legacy shutdown 之后 CPU 没有停机";
    EXPECT_TRUE(f.cpu->halted());
}

// ============================================================
// 坑 3：SBI TIME 只声明不产生 STIP → 等定时器中断的代码永久挂起
// ============================================================
TEST(LinuxBoot, SbiSetTimerRaisesStip) {
    LinuxFixture f;
    // M 态：mideleg.STIP=1 后，到期时刻必须反映到 sip.STIP
    f.words(0x00, {
        lui(1, 0x1),
        addi(1, 1, -2048),        // mstatus.MPP=S
        csrrw(0, CSR_MSTATUS, 1),
        addi(1, 0, 0x20),         // STIP 委托位
        csrrw(0, CSR_MIDELEG, 1),
        addi(1, 0, 0x40),
        csrrw(0, CSR_MEPC, 1),
        MRET,
    });
    std::vector<uint32_t> s;
    s.push_back(addi(16, 0, 0)); // a6 = fid 0 (set_timer)
    f.loadImm(17, SBI_EXT_TIME, s);
    s.push_back(addi(10, 0, 1)); // deadline = 1，早过当前 cycles
    s.push_back(ECALL);
    s.push_back(csr_rd(5, CSR_SIP));
    s.push_back(sw(5, 0, 0x100));
    s.push_back(spin());
    uint32_t a = 0x40;
    for (uint32_t v : s) {
        f.word(a, v);
        a += 4;
    }
    f.run(400);
    EXPECT_TRUE((f.mem32(0x100) & 0x20u) != 0)
        << "set_timer 到期后 sip.STIP 必须置位，否则内核的时钟中断永远不来";
}

// ============================================================
// 坑 4：sscratch/mscratch 用 32 位存，内核栈虚拟地址被截断成物理地址
// ============================================================
TEST(LinuxBoot, SscratchHoldsFull64Bit) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(1, 0, -1),
        enc_shift_i(1, 5, 1, 31), // srli x1,x1,31 → 1
        enc_shift_i(1, 1, 1, 63), // slli x1,x1,63 → 0x8000000000000000
        lui(2, 0x80184),
        addi(2, 2, 0x4C0),        // x2 = 0x801844C0
        or_(1, 1, 2),             // x1 = 0xFFFFFFFF801844C0（内核的栈地址形态）
        csrrw(0, CSR_SSCRATCH, 1),
        csr_rd(3, CSR_SSCRATCH),
        spin(),
    });
    f.run(400);
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0xFFFFFFFF801844C0ull)
        << "sscratch 被截断：陷阱入口拿到的 tp 会变成物理地址，一存就 page fault";
}

TEST(LinuxBoot, MscratchHoldsFull64Bit) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, -1),
        enc_shift_i(1, 5, 1, 31),
        enc_shift_i(1, 1, 1, 63),
        lui(2, 0x80184),
        addi(2, 2, 0x4C0),
        or_(1, 1, 2),
        csrrw(0, CSR_MSCRATCH, 1),
        csr_rd(3, CSR_MSCRATCH),
        spin(),
    });
    f.run(200);
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0xFFFFFFFF801844C0ull);
}

// ============================================================
// 坑 5：mstatus/sstatus 按 32 位处理，写高位把低位掩掉、读不到 UXL/SD
// ============================================================
TEST(LinuxBoot, MstatusWriteKeepsLowWritableBits) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, -1),          // 全 1
        csrrw(0, CSR_MSTATUS, 1),
        csr_rd(3, CSR_MSTATUS),
        sw(3, 0, 0x100),
        lui(1, 0x2),             // x1 = 0x2000 → FS=1
        csrrw(0, CSR_MSTATUS, 1),
        csr_rd(4, CSR_MSTATUS),
        enc_shift_i(5, 5, 4, 32), // RV64 的 SD 在 bit63，取高 32 位来看
        sw(5, 0, 0x108),
        spin(),
    });
    f.run(200);
    // 可写位掩码写成常量，和实现里的掩码同步退化时这里会报出来
    EXPECT_EQ(f.mem32(0x100) & 0x007E79AAu, 0x007E79AAu)
        << "mstatus 的可写位没有全部写进去";
    EXPECT_TRUE((f.mem32(0x108) & 0x80000000u) != 0)
        << "FS 非 0 时 SD 必须为 1，软件靠它判断要不要保存浮点上下文";
}

TEST(LinuxBoot, SstatusReportsUxlAndKeepsSFields) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, -1),
        csrrw(0, CSR_SSTATUS, 1),
        csr_rd(4, CSR_SSTATUS),
        sw(4, 0, 0x104),                 // 低 32 位
        enc_shift_i(5, 5, 4, 32),
        sw(5, 0, 0x108),                 // bit32 起
        spin(),
    });
    f.run(200);
    EXPECT_EQ(f.mem32(0x104) & 0x000C6122u, 0x000C6122u)
        << "sstatus 的 SIE/SPIE/SPP/FS/SUM/MXR 等位没写进去";
    EXPECT_EQ(f.mem32(0x108) & 0x3u, 2u) << "RV64 的 UXL 必须读回 2（64 位）";
}

// ============================================================
// 坑 6：mcounteren 复位为 0 → S 态第一条 rdtime 就非法指令
// ============================================================
TEST(LinuxBoot, SrdtimeWorksAfterReset) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        csr_rd(5, CSR_TIME),   // rdtime
        sw(5, 0, 0x100),
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.cpu->priv(), PRIV_S) << "S 态 rdtime 触发了陷阱";
    EXPECT_EQ(f.cpu->mcause(), 0u) << "S 态 rdtime 不该产生任何异常";
    EXPECT_NE(f.mem32(0x100), 0u) << "time 恒为 0 会让基于 rdtime 的超时逻辑静默失效";
}

TEST(LinuxBoot, SrdtimeTrapsWhenMcounterenCleared) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, 0x80),
        csrrw(0, CSR_MTVEC, 1),      // 陷阱处理程序
        csrrw(0, CSR_MCOUNTEREN, 0), // 把 TM 位关掉
        lui(1, 0x1),
        addi(1, 1, -2048),
        csrrw(0, CSR_MSTATUS, 1),
        addi(1, 0, 0x40),
        csrrw(0, CSR_MEPC, 1),
        MRET,
    });
    f.words(0x40, {
        csr_rd(5, CSR_TIME),         // 应当在这里陷入 M 态
        spin(),
    });
    f.words(0x80, {spin()});
    f.run(300);
    EXPECT_EQ(f.cpu->mcause(), 2u) << "mcounteren.TM=0 时 S 态读 time 必须报非法指令";
    EXPECT_EQ(f.cpu->mepc(), 0x40u) << "mepc 必须指向触发陷阱的那条 rdtime";
    EXPECT_EQ(f.cpu->priv(), PRIV_M);
}

// ============================================================
// 坑 7：misa 误报 H 扩展，内核走 has_hypervisor 分支
// ============================================================
TEST(LinuxBoot, MisaDoesNotClaimHypervisor) {
    LinuxFixture f;
    f.words(0x00, {
        csr_rd(5, CSR_MISA),
        sw(5, 0, 0x100),
        enc_shift_i(6, 5, 5, 32),
        sw(6, 0, 0x104),
        spin(),
    });
    f.run(200);
    const uint32_t lo = f.mem32(0x100);
    EXPECT_EQ((lo >> 21) & 1u, 0u) << "misa.H 是虚拟化扩展，本核没有实现";
    EXPECT_TRUE((lo & (1u << 8)) != 0) << "必须报告 I 扩展";
    EXPECT_TRUE((lo & (1u << 12)) != 0) << "必须报告 M 扩展";
    EXPECT_TRUE((lo & (1u << 18)) != 0) << "必须报告 S 模式";
    EXPECT_TRUE((lo & (1u << 20)) != 0) << "必须报告 U 模式";
    EXPECT_EQ((f.mem32(0x104) >> 30) & 3u, 2u) << "RV64 的 MXL 必须是 2";
}

// ============================================================
// 坑 8：LR/SC 的 cmpxchg 语义（内核自旋锁的底座）
// ============================================================
TEST(LinuxBoot, LrScCmpxchgSucceeds) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, 0x200),       // 目标地址
        addi(2, 0, 0),           // 期望旧值
        addi(3, 0, 1),           // 新值
        lr_w(4, 1),              // 0x0C: loop
        enc_b(1, 4, 2, 12),      // 0x10: 旧值不符 → 0x1C
        sc_w(5, 1, 3),           // 0x14
        enc_b(1, 5, 0, -12),     // 0x18: SC 失败 → 0x0C
        sw(5, 0, 0x220),         // 0x1C: 记录 SC 结果
        sw(4, 0, 0x224),
        spin(),
    });
    f.run(300);
    EXPECT_EQ(f.mem32(0x200), 1u) << "cmpxchg 没写进去，内核自旋锁永远拿不到";
    EXPECT_EQ(f.mem32(0x220), 0u) << "SC 必须成功返回 0";
}

// ============================================================
// 坑 9：逐字节大块复制（内核 __memcpy 的尾部路径）
// ============================================================
TEST(LinuxBoot, ByteCopyTwoHundredBytes) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, 0x400),       // src
        addi(2, 0, 0x600),       // dst
        addi(3, 0, 200),         // len
        add(4, 1, 3),            // end = src + len
        lb(5, 1, 0),             // 0x10: loop
        sb(5, 2, 0),
        addi(1, 1, 1),
        addi(2, 2, 1),
        enc_b(1, 1, 4, -16),     // 0x20: 回到 0x10
        spin(),
    });
    uint8_t *m = f.mem->writableMemPtr();
    for (int i = 0; i < 200; i++) m[0x400 + i] = (uint8_t) ('A' + (i % 26));
    m[0x600 + 200] = 0x5A;
    // 每轮循环 5 条指令、每条要走取指/执行多个相位，200 轮要几千个 tick
    f.run(4000);
    bool ok = true;
    for (int i = 0; i < 200; i++) ok = ok && (m[0x600 + i] == m[0x400 + i]);
    EXPECT_TRUE(ok) << "200 字节复制出现错位/丢字节";
    EXPECT_EQ(m[0x600 + 200], 0x5A) << "复制越界写到了目标缓冲区之外";
}

// ============================================================
// 坑 11：legacy console_putchar（a7=1）同样走 sbiret，且不能吃掉后面的指令
// ============================================================
TEST(LinuxBoot, SbiLegacyConsolePutchar) {
    LinuxFixture f;
    f.enterS(0x40);
    f.words(0x40, {
        addi(17, 0, 1),      // a7 = legacy console_putchar
        addi(10, 0, 'X'),    // 字符在 a0
        ECALL,
        sw(10, 0, 0x100),    // 成功时 a0 必须是 0
        addi(5, 0, 0x42),
        sw(5, 0, 0x104),
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0u);
    EXPECT_EQ(f.mem32(0x104), 0x42u) << "console_putchar 之后没有回到下一条指令";
}

// ============================================================
// 坑 12：SRST 关机后必须真的停机，否则内核 reboot 流程会继续跑
// ============================================================
TEST(LinuxBoot, SbiSrstHalts) {
    LinuxFixture f;
    std::vector<uint32_t> s;
    s.push_back(addi(16, 0, 0)); // fid = reset
    f.loadImm(17, 0x53525354ull, s); // "SRST"
    s.push_back(ECALL);
    s.push_back(addi(5, 0, 0x42));
    s.push_back(sw(5, 0, 0x100)); // 停机之后这条不能被执行
    s.push_back(spin());
    f.enterS(0x40);
    uint32_t a = 0x40;
    for (uint32_t v : s) {
        f.word(a, v);
        a += 4;
    }
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 0u) << "SRST 之后 CPU 没有停机，后续指令仍在执行";
    EXPECT_TRUE(f.cpu->halted());
}

// ============================================================
// 坑 13：M 态装了陷阱向量时，S 态 ecall 必须按规范陷入 M 态（内建 SBI 让位给固件）
// ============================================================
TEST(LinuxBoot, SEcallTrapsWhenMtvecInstalled) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, 0x80),
        csrrw(0, CSR_MTVEC, 1),
        lui(1, 0x1),
        addi(1, 1, -2048),           // MPP=S
        csrrw(0, CSR_MSTATUS, 1),
        addi(1, 0, 0x40),
        csrrw(0, CSR_MEPC, 1),
        MRET,
    });
    f.words(0x40, {ECALL});
    f.words(0x80, {
        csr_rd(5, CSR_MCAUSE),
        sw(5, 0, 0x100),
        spin(),
    });
    f.run(400);
    EXPECT_EQ(f.mem32(0x100), 9u) << "有 M 态固件时 S 态 ecall 的 mcause 必须是 9";
    EXPECT_EQ(f.cpu->priv(), PRIV_M);
}


// ============================================================
// 坑 15：OP-IMM-32 的 inst[31:25] 是立即数高位，不是 funct7。
// addiw 的立即数 0x81 会让它恰好等于 slli.uw 的 funct7(0x04)，
// 一旦先按 funct7 分流就会把普通 addiw 判成非法指令（内核就是这么挂的）。
// 这里把 4096 种立即数全跑一遍，任何一类被误判都会在这里暴露
// ============================================================
TEST(LinuxBoot, AddiwAcceptsEveryImmediate) {
    LinuxFixture f;
    std::vector<uint32_t> prog;
    for (int imm = 0; imm < 4096; imm++) {
        const int32_t s = (imm >= 2048) ? imm - 4096 : imm;
        prog.push_back(addiw(10, 10, s));
    }
    prog.push_back(spin());
    uint32_t a = 0;
    for (uint32_t v : prog) {
        f.word(a, v);
        a += 4;
    }
    f.run((uint32_t) (prog.size() * 4 + 400));
    uint64_t expect = 0;
    for (int imm = 0; imm < 4096; imm++) {
        const int32_t s = (imm >= 2048) ? imm - 4096 : imm;
        const uint32_t lo = (uint32_t) ((uint32_t) expect + (uint32_t) s);
        expect = (uint64_t) (int64_t) (int32_t) lo;
    }
    EXPECT_EQ(f.cpu->mcause(), 0u) << "addiw 的某些立即数被误判成了 Zba/Zbb 扩展指令";
    EXPECT_EQ((uint64_t) f.cpu->reg(10), expect) << "addiw 结果不对，说明有立即数走了错误分支";
}

// OP-IMM 同理：addi 的立即数高位也会伪装成 funct7/立即数高位
TEST(LinuxBoot, AddiAcceptsEveryImmediate) {
    LinuxFixture f;
    std::vector<uint32_t> prog;
    for (int imm = 0; imm < 4096; imm++) {
        const int32_t s = (imm >= 2048) ? imm - 4096 : imm;
        prog.push_back(addi(10, 10, s));
    }
    prog.push_back(spin());
    uint32_t a = 0;
    for (uint32_t v : prog) {
        f.word(a, v);
        a += 4;
    }
    f.run((uint32_t) (prog.size() * 4 + 400));
    uint64_t expect = 0;
    for (int imm = 0; imm < 4096; imm++)
        expect += (uint64_t) (int64_t) ((imm >= 2048) ? imm - 4096 : imm);
    EXPECT_EQ(f.cpu->mcause(), 0u) << "addi 的某些立即数被误判成了扩展指令";
    EXPECT_EQ((uint64_t) f.cpu->reg(10), expect);
}

// ============================================================
// 坑 14：U 态 ecall 必须按 medeleg 委托到 S 态（内核用它做系统调用）
// ============================================================
TEST(LinuxBoot, UserEcallDelegatesToS) {
    LinuxFixture f;
    f.words(0x00, {
        addi(1, 0, 0x100),       // medeleg.bit8 = ECALL_U
        csrrw(0, CSR_MEDELEG, 1),
        addi(1, 0, 0x80),        // stvec
        csrrw(0, CSR_STVEC, 1),
        lui(1, 0x1),
        addi(1, 1, -2048),       // MPP=S
        csrrw(0, CSR_MSTATUS, 1),
        addi(1, 0, 0x40),
        csrrw(0, CSR_MEPC, 1),
        MRET,
    });
    f.words(0x40, {
        addi(1, 0, 0x20),        // sstatus.SPIE=1，SPP=0 → sret 回 U 态
        csrrw(0, CSR_SSTATUS, 1),
        addi(1, 0, 0xC0),
        csrrw(0, CSR_SEPC, 1),
        SRET,
    });
    f.words(0x80, {
        csr_rd(5, CSR_SCAUSE),
        sw(5, 0, 0x700),
        csr_rd(6, CSR_SEPC),
        sw(6, 0, 0x704),
        spin(),
    });
    f.words(0xC0, {ECALL});
    f.run(500);
    EXPECT_EQ(f.mem32(0x700), 8u) << "U 态 ecall 的 scause 必须是 8";
    EXPECT_EQ(f.mem32(0x704), 0xC0u) << "sepc 必须指向那条 ecall";
    EXPECT_EQ(f.cpu->priv(), PRIV_S) << "陷阱应当落在 S 态";
}

} // namespace
