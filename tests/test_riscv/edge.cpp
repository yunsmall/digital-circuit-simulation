// RV32 的边缘场景：官方测试覆盖不到、或者覆盖了也说不清行为的地方。
#include <gtest/gtest.h>

#include "platform.h"

namespace {

constexpr uint32_t i_type(int opcode, int rd, int f3, int rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | ((rs1 & 0x1F) << 15)
         | ((f3 & 7) << 12) | ((rd & 0x1F) << 7) | (opcode & 0x7F);
}
constexpr uint32_t addi_(int rd, int rs1, int32_t imm) { return i_type(0x13, rd, 0, rs1, imm); }
constexpr uint32_t lui_(int rd, uint32_t imm20) { return (imm20 << 12) | ((rd & 0x1F) << 7) | 0x37; }
constexpr uint32_t csrrs_(int rd, int csr, int rs1) { return i_type(0x73, rd, 2, rs1, csr); }
constexpr uint32_t csrrw_(int rd, int csr, int rs1) { return i_type(0x73, rd, 1, rs1, csr); }
constexpr uint32_t lw_(int rd, int rs1, int32_t imm) { return i_type(0x03, rd, 2, rs1, imm); }
constexpr uint32_t sw_(int rs1, int rs2, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((rs2 & 0x1F) << 20)
         | ((rs1 & 0x1F) << 15) | (2u << 12) | ((uint32_t)(imm & 0x1F) << 7) | 0x23u;
}
constexpr uint32_t amo_(int rd, int rs1, int rs2, uint32_t funct5) {
    return (funct5 << 27) | ((rs2 & 0x1F) << 20) | ((rs1 & 0x1F) << 15)
         | (2u << 12) | ((rd & 0x1F) << 7) | 0x2Fu;
}
constexpr uint32_t spin_() { return 0x0000006Fu; }

} // namespace

// zip/unzip 的 rs2 是固定编码 0x0F。funct3=1/5 且 funct7=0x04 的其它 rs2
// 是保留编码，必须触发非法指令，不能当成 zip/unzip 静默执行
TEST(Rv32Edge, ReservedZipEncodingTraps) {
    PrivTestFixture f;
    f.loadProgram({
        addi_(1, 0, 0x20),                 // mtvec = 0x20
        csrrw_(0, 0x305, 1),
        i_type(0x13, 2, 1, 0, 0x080),      // f3=1、f7=0x04、rs2=0：保留编码
        addi_(0, 0, 0), addi_(0, 0, 0), addi_(0, 0, 0), addi_(0, 0, 0), addi_(0, 0, 0),
        csrrs_(3, 0x342, 0),                // mcause
        csrrs_(4, 0x341, 0),                // mepc
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(120);
    EXPECT_EQ(f.cpu->reg(3), 2u) << "保留编码应报非法指令";
    EXPECT_EQ(f.cpu->reg(4), 0x08u) << "mepc 应指向保留编码本身";
    EXPECT_EQ(f.cpu->reg(2), 0u) << "保留编码不能产生 zip 结果";
}

// rs2=0x0F 的编码才是真正的 zip：RV32 下 zip(1) 把两个半区的 bit0 交错成 0b11
TEST(Rv32Edge, ZipWithFixedRs2Executes) {
    PrivTestFixture f;
    f.loadProgram({
        lui_(1, 0x10), addi_(1, 1, 1),     // x1 = 0x00010001
        i_type(0x13, 2, 1, 1, 0x08F),      // zip x2, x1
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(100);
    EXPECT_EQ(f.cpu->reg(2), 3u);
}

// MPP 是 WARL：保留值 2 不能原样读回。本核把它按 M（3）保存，
// 和 mret 对 2 的处理保持一致
TEST(Rv32Edge, MstatusMppReservedValueIsLegalized) {
    PrivTestFixture f;
    f.loadProgram({
        lui_(1, 1),                 // x1 = 2 << 11（MPP=2，保留编码）
        csrrw_(0, 0x300, 1),        // mstatus = x1
        csrrs_(2, 0x300, 0),        // x2 = mstatus
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(100);
    EXPECT_EQ(((uint32_t) f.cpu->reg(2) >> 11) & 3u, 3u) << "MPP 读回必须是合法值";
}

// AMOMIN/AMOMAX 必须按有符号比较：INT32_MIN 和 1 比，signed min 是 INT32_MIN
TEST(Rv32Edge, AmoMinMaxUseSignedCompareAtInt32Min) {    PrivTestFixture f;
    f.loadProgram({
        lui_(1, 0x1), addi_(1, 1, 0x100),          // x1 = 0x1100
        lui_(2, 0x80000),                          // x2 = 0x80000000（INT32_MIN）
        sw_(1, 2, 0),                              // sw x2, 0(x1)
        addi_(3, 0, 1),                            // x3 = 1
        amo_(4, 1, 3, 0x10),                       // amomin.w x4, x3, (x1)
        lw_(5, 1, 0),                              // lw x5, 0(x1)
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(200);
    EXPECT_EQ((uint32_t) f.cpu->reg(4), 0x80000000u) << "AMOMIN 的旧值不对";
    EXPECT_EQ((uint32_t) f.cpu->reg(5), 0x80000000u) << "AMOMIN 用了无符号比较";
}

// 未实现的 CSR 必须报非法指令，不能静默返回 0
TEST(Rv32Edge, UnimplementedCsrTraps) {
    PrivTestFixture f;
    f.loadProgram({
        csrrs_(1, 0x7C0, 0),   // 本核不实现的 CSR
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(40);
    EXPECT_EQ(f.cpu->pc(), 0u) << "非法 CSR 没有 trap";
    EXPECT_EQ((uint32_t) f.cpu->reg(1), 0u) << "非法 CSR 不该写回 rd";
}

// AMO.D 是 RV64 专用指令，RV32 上执行必须报非法指令而不是按 64 位原子操作执行
TEST(Rv32Edge, AmoDOnRv32Traps) {
    PrivTestFixture f;
    const uint32_t amoswap_d = (1u << 27) | (2u << 20) | (1u << 15)
                             | (3u << 12) | (4u << 7) | 0x2Fu;   // amoswap.d x4, x2, (x1)
    f.loadProgram({
        lui_(1, 0x1), addi_(1, 1, 0x100),
        addi_(2, 0, 7),
        amoswap_d,
        addi_(5, 0, 1),             // 只有没 trap 才会执行到
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(80);
    EXPECT_EQ((uint32_t) f.cpu->reg(5), 0u) << "RV32 上的 AMO.D 没有 trap";
    EXPECT_EQ((uint32_t) f.cpu->reg(4), 0u) << "非法 AMO.D 不该写回 rd";
}

// FCVT.L.D / FCVT.LU.D 是 RV64 专用指令，RV32 上必须报非法指令
TEST(Rv32Edge, FcvtLdOnRv32Traps) {
    PrivTestFixture f;
    const uint32_t fcvt_l_d = (0x61u << 25) | (2u << 20) | (1u << 15)
                            | (2u << 7) | 0x53u;              // fcvt.l.d x2, f1（rs2=2 选 L）
    f.loadProgram({
        lui_(1, 2),                       // x1 = 0x2000：mstatus.FS = Initial
        csrrw_(0, 0x300, 1),
        fcvt_l_d,
        addi_(3, 0, 1),                   // 只有没 trap 才会执行到
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(80);
    EXPECT_EQ((uint32_t) f.cpu->reg(3), 0u) << "RV32 上的 FCVT.L.D 没有 trap";
    EXPECT_EQ((uint32_t) f.cpu->reg(2), 0u) << "非法 FCVT.L.D 不该写回 rd";
}

// 清掉 mideleg.STIP 后不能残留上一次镜像出来的 mip.STIP：
// 那个位反映的是"是否委托"，委托撤销后继续挂起会让 M 态收到假的 S 级中断
TEST(Rv32Edge, StipClearsWhenDelegationRemoved) {
    PrivTestFixture f;
    f.loadProgram({
        addi_(2, 0, 0x20),          // STIP
        csrrw_(0, 0x303, 2),        // mideleg.STIP = 1
        addi_(0, 0, 0), addi_(0, 0, 0),
        csrrw_(0, 0x303, 0),        // 撤销委托
        addi_(0, 0, 0), addi_(0, 0, 0),
        csrrs_(3, 0x344, 0),        // x3 = mip
        spin_(),
    });
    f.compile(); f.reset();
    f.circuit->setWire(f.mti_net->id(), 1);   // 定时器中断源：委托时会被镜像成 STIP
    f.runCycles(120);
    EXPECT_EQ((uint32_t) f.cpu->reg(3) & 0x20u, 0u)
        << "撤销 mideleg.STIP 后 mip.STIP 仍然挂起";
}

// 规范 3.1.8：STI 委托给 S 后，M 态执行时不得接走它（要等回到 S 态再处理）。
// 这里 M 态开 MIE、开 mie.STIP，STIP 已挂起；正确行为是一直跑到末尾的自旋，
// 而不是 trap 到 0x100
TEST(Rv32Edge, MModeIgnoresDelegatedSInterrupt) {
    PrivTestFixture f;
    f.loadProgram({
        addi_(1, 0, 0x100),         // 0x00
        csrrw_(0, 0x305, 1),        // mtvec = 0x100
        addi_(2, 0, 8),             // mstatus.MIE
        csrrw_(0, 0x300, 2),
        addi_(2, 0, 0x20),          // mideleg.STIP | mie.STIP
        csrrw_(0, 0x303, 2),
        csrrw_(0, 0x304, 2),
        addi_(0, 0, 0), addi_(0, 0, 0), addi_(0, 0, 0), addi_(0, 0, 0),
        spin_(),                    // 0x28
    });
    f.compile(); f.reset();
    f.circuit->setWire(f.mti_net->id(), 1);   // 定时器中断源
    f.runCycles(160);
    EXPECT_EQ(f.cpu->pc(), 0x2Cu) << "M 态接走了已委托给 S 的定时器中断";
}

// RV32 非对齐 8 字节存储会拆成三段总线事务，段与段之间 stb 必须真的拉低；
// 第二段的 sel 若保留 5 位，bit4 会串到 stb 位上，产生一个多余的事务
// （RV32 的 SD 是非法指令，这条用例放在 test_riscv64/edge.cpp）

// CSR 只读字段写不进去：misa 的 MXL 必须恒为 1（RV32）
TEST(Rv32Edge, MisaMxlStaysOne) {
    PrivTestFixture f;
    f.loadProgram({
        addi_(1, 0, -1),
        csrrw_(0, 0x301, 1),   // 往 misa 写全 1
        csrrs_(2, 0x301, 0),   // x2 = misa
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(60);
    EXPECT_EQ(((uint32_t) f.cpu->reg(2) >> 30) & 3u, 1u) << "misa 的 MXL 被写坏了";
}

// SC 成功路径：LR 之后没人动过保留地址，SC 返回 0 并写入
TEST(Rv32Edge, ScSucceedsWhenReservationIntact) {
    PrivTestFixture f;
    f.loadProgram({
        lui_(1, 0x1), addi_(1, 1, 0x100),
        addi_(2, 0, 7),
        sw_(1, 2, 0),                              // sw x2, 0(x1)
        amo_(4, 1, 0, 0x02),                       // lr.w x4, (x1)
        addi_(5, 0, 9),
        amo_(6, 1, 5, 0x03),                       // sc.w x6, x5, (x1)
        lw_(7, 1, 0),
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(240);
    EXPECT_EQ((uint32_t) f.cpu->reg(6), 0u) << "保留有效时 SC 却失败了";
    EXPECT_EQ((uint32_t) f.cpu->reg(7), 9u) << "SC 成功但内存没更新";
}
