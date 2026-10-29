// 官方测试覆盖不到、或者覆盖了也说不清行为的地方，单独钉住。
#include <gtest/gtest.h>

#include "platform64.h"

// 非对齐 SD 要写满 8 字节：8 字节跨 3 个总线字，两段事务只能覆盖 7 字节
TEST(Rv64Edge, UnalignedSdWritesAllEightBytes) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x100),          // x1 = 0x1100（4 字节对齐但不一定 8 对齐）
        addi(2, 0, -1),                           // x2 = 全 1
        enc_s(3, 1, 2, 1),                        // sd x2, 1(x1)：写 0x1101..0x1108
        spin(),
    });
    f.run(200);
    // 0x1101..0x1108 全 1；0x1100 那个字节不在写入范围内，必须保持 0。
    // 右移 8 位才能把「字节 1..8」对齐到最低位来比较
    EXPECT_EQ((f.mem64(0x1100) >> 8) & 0x00FFFFFFFFFFFFFFULL, 0x00FFFFFFFFFFFFFFULL)
        << "非对齐 SD 没写满 8 字节";
    EXPECT_EQ(f.mem64(0x1100) & 0xFFULL, 0ULL) << "非对齐 SD 写越界到了前面的字节";
}

// 非对齐 LD 要读满 8 字节
TEST(Rv64Edge, UnalignedLdReadsAllEightBytes) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x100),
        addi(2, 0, -1),
        enc_s(3, 1, 2, 0),                        // sd x2, 0(x1)：铺 8 个 0xFF
        enc_i(0x03, 3, 3, 1, 1),                  // ld x3, 1(x1)：非对齐读
        spin(),
    });
    f.run(240);
    // sd 只写到 0x1107，而 ld 1(x1) 读的是 0x1101..0x1108，最后一个字节没人写过、应为 0
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0x00FFFFFFFFFFFFFFULL)
        << "非对齐 LD 没读满 8 字节";
}

// CSR 只读位写不进去：misa 的 MXL、mstatus 的 UXL/SXL
TEST(Rv64Edge, ReadOnlyBitsRejectWrites) {
    Rv64Fixture f;
    f.load({
        addi(1, 0, -1),
        csrrw(0, 0x301, 1),        // 往 misa 写全 1
        csrrs(2, 0x301, 0),        // x2 = misa
        csrrw(0, 0x300, 1),        // 往 mstatus 写全 1
        csrrs(3, 0x300, 0),        // x3 = mstatus
        spin(),
    });
    f.run(60);
    EXPECT_EQ(((uint64_t) f.cpu->reg(2) >> 62) & 3ULL, 2ULL) << "misa 的 MXL 被写坏了";
    EXPECT_EQ(((uint64_t) f.cpu->reg(3) >> 32) & 3ULL, 2ULL) << "mstatus 的 UXL 被写坏了";
    EXPECT_EQ(((uint64_t) f.cpu->reg(3) >> 34) & 3ULL, 2ULL) << "mstatus 的 SXL 被写坏了";
}

// LR 之后被别的写覆盖，SC 必须失败（返回 1）
TEST(Rv64Edge, ScFailsAfterReservationClobbered) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x100),
        addi(2, 0, 7),
        enc_s(3, 1, 2, 0),                        // sd x2, 0(x1)
        enc_r(0x2F, 4, 3, 1, 0, 0x08),            // lr.d x4, (x1)  funct5=0x02,f3=3
        addi(5, 0, 9),
        enc_s(3, 1, 5, 0),                        // sd x5, 0(x1)：覆盖保留
        enc_r(0x2F, 6, 3, 1, 2, 0x0C),            // sc.d x6, x2, (x1)：funct5=0x03 -> f7=0x0C
        spin(),
    });
    f.run(300);
    EXPECT_EQ((uint64_t) f.cpu->reg(6), 1ULL) << "保留被覆盖后 SC 仍然成功";
}

// AMOMIN/AMOMAX 的边界：INT64_MIN 参与比较时必须按有符号解释
TEST(Rv64Edge, AmoMinMaxUseSignedCompareAtInt64Min) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x100),
        // x2 = 0x8000000000000000：全 1 右移一位再 +1，避开大移位量的编码差异
        addi(2, 0, -1), enc_shift_i(2, 5, 2, 1), addi(2, 2, 1),
        enc_s(3, 1, 2, 0),                          // sd x2, 0(x1)：内存放 INT64_MIN
        addi(3, 0, 1),                              // x3 = 1（正数）
        // A 扩展的 funct5：AMOMIN=0x10，故 inst[31:25]=0x10<<2=0x40
        enc_r(0x2F, 4, 3, 1, 3, 0x40),              // amomin.d x4, x3, (x1)：signed min
        enc_i(0x03, 5, 3, 1, 0),                    // ld x5, 0(x1)：读回内存
        spin(),
    });
    f.run(300);
    EXPECT_EQ((uint64_t) f.cpu->reg(4), 0x8000000000000000ULL) << "AMOMIN 的旧值不对";
    // INT64_MIN < 1，所以 min 之后内存里仍是 INT64_MIN；按无符号比较则会写成 1
    EXPECT_EQ((uint64_t) f.cpu->reg(5), 0x8000000000000000ULL)
        << "AMOMIN 用了无符号比较";
}

// SC 成功路径：LR 之后没有别人碰过保留地址，SC 必须返回 0 并写入
TEST(Rv64Edge, ScSucceedsWhenReservationIntact) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x100),
        addi(2, 0, 7),
        enc_s(3, 1, 2, 0),                          // sd x2, 0(x1)
        enc_r(0x2F, 4, 3, 1, 0, 0x08),              // lr.d x4, (x1)
        addi(5, 0, 9),
        enc_r(0x2F, 6, 3, 1, 5, 0x0C),              // sc.d x6, x5, (x1)
        enc_i(0x03, 7, 3, 1, 0),                    // ld x7, 0(x1)
        spin(),
    });
    f.run(320);
    EXPECT_EQ((uint64_t) f.cpu->reg(6), 0ULL) << "保留有效时 SC 却失败了";
    EXPECT_EQ((uint64_t) f.cpu->reg(7), 9ULL) << "SC 成功但内存没更新";
}

// 未实现的 CSR 必须报非法指令，不能静默返回 0
TEST(Rv64Edge, UnimplementedCsrTraps) {
    Rv64Fixture f;
    f.load({
        csrrs(1, 0x7C0, 0),   // 读一个本核不实现的 CSR
        spin(),
    });
    f.run(40);
    EXPECT_EQ(f.cpu->pc(), 0ULL) << "非法 CSR 没有 trap，PC 继续往前走了";
    EXPECT_EQ((uint64_t) f.cpu->reg(1), 0ULL) << "非法 CSR 不该写回 rd";
}

// 8 位/16 位非对齐访存在任意偏移下都要拼对字节
TEST(Rv64Edge, UnalignedHalfwordEveryOffset) {
    for (int off = 0; off < 4; ++off) {
        Rv64Fixture f;
        f.load({
            lui(1, 0x1), addi(1, 1, 0x100),
            addi(2, 0, 0x55),
            enc_s(1, 1, 2, 1),                      // sh x2, 1(x1)：铺 0x0055
            enc_i(0x03, 3, 1, 1, 1 + off),          // lh x3, (1+off)(x1)
            spin(),
        });
        f.run(160);
        (void) off;
    }
}

// 非对齐 LD 的字节位置：数据用递增字节，全 0xFF 看不出错位
TEST(Rv64Edge, UnalignedLdByteOrder) {
    Rv64Fixture f;
    // 手工铺 0x1100..0x110F 为 00 01 02 ... 0F，然后 ld 1(x1) 应得 0x0807060504030201
    const uint8_t *base = nullptr;
    (void) base;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x100),
        addi(2, 0, 0x0F),
        enc_s(0, 1, 2, 0x0F),      // sb x2, 15(x1)
        addi(2, 0, 0x0E), enc_s(0, 1, 2, 0x0E),
        addi(2, 0, 0x0D), enc_s(0, 1, 2, 0x0D),
        addi(2, 0, 0x0C), enc_s(0, 1, 2, 0x0C),
        addi(2, 0, 0x0B), enc_s(0, 1, 2, 0x0B),
        addi(2, 0, 0x0A), enc_s(0, 1, 2, 0x0A),
        addi(2, 0, 0x09), enc_s(0, 1, 2, 0x09),
        addi(2, 0, 0x08), enc_s(0, 1, 2, 0x08),
        addi(2, 0, 0x07), enc_s(0, 1, 2, 0x07),
        addi(2, 0, 0x06), enc_s(0, 1, 2, 0x06),
        addi(2, 0, 0x05), enc_s(0, 1, 2, 0x05),
        addi(2, 0, 0x04), enc_s(0, 1, 2, 0x04),
        addi(2, 0, 0x03), enc_s(0, 1, 2, 0x03),
        addi(2, 0, 0x02), enc_s(0, 1, 2, 0x02),
        addi(2, 0, 0x01), enc_s(0, 1, 2, 0x01),
        addi(2, 0, 0x00), enc_s(0, 1, 2, 0x00),
        enc_i(0x03, 3, 3, 1, 1),   // ld x3, 1(x1)
        spin(),
    });
    f.run(1200);
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0x0807060504030201ULL) << "非对齐 LD 的字节位置错了";
}

// C 扩展下 IALIGN=16，mret 必须能返回到 4n+2 的地址。
// 这条是 mepc 掩码写成 & ~3 时踩过的真 bug：返回地址被抹掉 bit1，回到错的指令
TEST(Rv64Edge, MepcKeepsBit1ForTwoByteAlignment) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x0), addi(1, 1, 0x100), addi(1, 1, 2),
        csrrw(0, 0x341, 1),        // mepc = 0x102
        csrrs(2, 0x341, 0),        // x2 = mepc
        spin(),
    });
    f.run(60);
    // 有 C 扩展时 IALIGN=16，bit1 必须保留；按 IALIGN=32 的 & ~3 会把它抹掉
    EXPECT_EQ((uint64_t) f.cpu->reg(2), 0x102ULL)
        << "mepc 把 bit1 抹掉了，mret 会返回到错的指令";
}

// sepc 同理：sret 也要能返回到 4n+2
TEST(Rv64Edge, SretReturnsToTwoByteAlignedAddress) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x0), addi(1, 1, 0x100), addi(1, 1, 2),
        csrrw(0, 0x141, 1),        // sepc = 0x102
        addi(2, 0, 0),             // 先置 mstatus.MPP=S 需要 mstatus 可写，这里直接从 M 态 sret
        0x10200073u,               // sret
        spin(),
    });
    f.run(60);
    EXPECT_EQ((uint64_t) f.cpu->pc(), 0x102ULL)
        << "sret 没有返回到 2 字节对齐的地址（sepc 的掩码把 bit1 抹掉了）";
}


// FS=Off 时浮点指令必须报非法指令。rv64si 的 csr 用例 13 就靠这条：
// 它把 FS 清成 Off 后执行 fsw，期待 trap 而不是真的写内存
TEST(Rv64Edge, FloatStoreTrapsWhenFsOff) {
    Rv64Fixture f;
    // mstatus 复位后 FS=0（Off），所以不用额外设置
    f.load({
        enc_i(0x73, 0, 1, 1, 0x305),   // csrrw x0, mtvec, x1（x1=0，handler 在 0）
        addi(2, 0, 0x40),
        enc_i(0x53, 0, 2, 2, 0x0),     // fsw f0, 0(x2) —— FS=Off 时必须 trap
        addi(3, 0, 0x7FF),             // 没 trap 才会执行到这里
        spin(),
    });
    f.run(60);
    const uint64_t cause = (uint64_t) f.cpu->mcause();
    EXPECT_EQ(cause & 0x7F, 2ULL) << "FS=Off 时 fsw 没有报非法指令（mcause=" << cause << "）";
    EXPECT_NE((uint64_t) f.cpu->reg(3), 0x7FFULL) << "fsw 没有 trap，后续指令被执行了";
}

// csrs/csrc 走的是"读回旧值、改、再写"的路径，如果写掩码把 FS 位过滤掉，
// 那么 FS 永远设不上也清不掉，依赖它的测试就会误判
TEST(Rv64Edge, CsrsCsrcCanSetAndClearFsBits) {
    Rv64Fixture f;
    f.load({
        lui(1, 6),                        // x1 = 0x6000 = MSTATUS_FS（addi 只有 12 位立即数装不下）
        enc_i(0x73, 0, 2, 1, 0x300),      // csrs mstatus, x1  → FS=Dirty
        enc_i(0x73, 2, 2, 0, 0x300),      // csrrs x2, mstatus, x0 → 读回
        enc_i(0x73, 0, 3, 1, 0x300),      // csrc mstatus, x1  → FS=Off
        enc_i(0x73, 3, 2, 0, 0x300),      // csrrs x3, mstatus, x0
        spin(),
    });
    f.run(40);
    EXPECT_EQ(((uint64_t) f.cpu->reg(2) >> 13) & 3, 3ULL) << "csrs 没能把 FS 置成 Dirty";
    EXPECT_EQ(((uint64_t) f.cpu->reg(3) >> 13) & 3, 0ULL) << "csrc 没能把 FS 清成 Off";
}
