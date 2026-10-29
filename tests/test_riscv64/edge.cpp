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
    // 0x1101..0x1108 全 1，两端的字节不受影响
    EXPECT_EQ(f.mem64(0x1101 - 1) & 0x00FFFFFFFFFFFFFFULL, 0x00FFFFFFFFFFFFFFULL)
        << "非对齐 SD 没写满 8 字节";
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
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0xFFFFFFFFFFFFFFFFULL)
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
