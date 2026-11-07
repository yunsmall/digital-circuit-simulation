// WishboneMemory 的从设备侧协议：重点是"同一个主设备连续发多笔事务"。
// RISC-V CPU 把非对齐访问拆成 2~3 笔事务，如果从设备在第三笔时不再锁存新地址，
// CPU 拿到的就是上一笔的数据。这个缺陷以前没有任何测试能发现——没有单独的内存
// 设备测试，全靠 CPU 的端到端用例间接覆盖，而端到端用例用的是对齐访问。
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>

#include "common/wb_master.h"

using namespace dsc;
using dsc::test::WbMaster;

namespace {

struct MemFixture {
    std::unique_ptr<Circuit> circuit;
    WishboneMemory *mem = nullptr;
    std::unique_ptr<WbMaster> bus;

    explicit MemFixture(int addr_bits = 16, int data_bits = 32) {
        circuit = std::make_unique<Circuit>();
        auto m = std::make_unique<WishboneMemory>("mem0", addr_bits, data_bits);
        mem = m.get();
        circuit->addComponent(std::move(m));
        bus = std::make_unique<WbMaster>(*circuit);
        bus->attach(mem);
        bus->finish();
    }

    uint8_t *bytes() { return mem->writableMemPtr(); }
};

} // namespace

TEST(WbMemory, SingleReadWrite) {
    MemFixture f;
    ASSERT_TRUE(f.bus->transact(0x100, true, 0xDEADBEEF, 0x0F)) << "写没有 ack";
    uint32_t v = 0;
    ASSERT_TRUE(f.bus->transact(0x100, false, 0, 0x0F, &v)) << "读没有 ack";
    EXPECT_EQ(v, 0xDEADBEEFu);
}

// 连续三笔读不同地址：每笔都必须返回各自地址的数据。
// CPU 拆非对齐 8 字节访问就是这种形态，第三笔拿到旧数据会直接算错结果
TEST(WbMemory, ThreeConsecutiveReadsReturnOwnData) {
    MemFixture f;
    uint8_t *m = f.bytes();
    m[0x100] = 0x00; m[0x101] = 0x01; m[0x102] = 0x02; m[0x103] = 0x03;
    m[0x104] = 0x04; m[0x105] = 0x05; m[0x106] = 0x06; m[0x107] = 0x07;
    m[0x108] = 0x08; m[0x109] = 0x09; m[0x10A] = 0x0A; m[0x10B] = 0x0B;

    uint32_t a = 0, b = 0, c = 0;
    ASSERT_TRUE(f.bus->transact(0x100, false, 0, 0x0F, &a));
    ASSERT_TRUE(f.bus->transact(0x104, false, 0, 0x0F, &b));
    ASSERT_TRUE(f.bus->transact(0x108, false, 0, 0x0F, &c));
    EXPECT_EQ(a, 0x03020100u) << "第一笔读错";
    EXPECT_EQ(b, 0x07060504u) << "第二笔读错";
    EXPECT_EQ(c, 0x0B0A0908u) << "第三笔没读到新地址";   // 字节 08 09 0A 0B 小端拼成 0x0B0A0908
}

TEST(WbMemory, FiveConsecutiveReads) {
    MemFixture f;
    uint8_t *m = f.bytes();
    for (int i = 0; i < 6; ++i)
        for (int b = 0; b < 4; ++b) m[0x200 + i * 4 + b] = (uint8_t)(i * 4 + b);
    for (int i = 0; i < 5; ++i) {
        uint32_t v = 0;
        ASSERT_TRUE(f.bus->transact(0x200 + i * 4, false, 0, 0x0F, &v)) << "第 " << i << " 笔没 ack";
        EXPECT_EQ(v, (uint32_t)(i * 4) | ((uint32_t)(i * 4 + 1) << 8)
                    | ((uint32_t)(i * 4 + 2) << 16) | ((uint32_t)(i * 4 + 3) << 24))
            << "第 " << i << " 笔读错";
    }
}

// 字节使能：只写低两字节，高两字节保持不变
TEST(WbMemory, PartialWriteRespectsSel) {
    MemFixture f;
    uint8_t *m = f.bytes();
    for (int i = 0; i < 4; ++i) m[0x300 + i] = 0xFF;
    ASSERT_TRUE(f.bus->transact(0x300, true, 0x0000AABB, 0x03));
    EXPECT_EQ(m[0x300], 0xBB);
    EXPECT_EQ(m[0x301], 0xAA);
    EXPECT_EQ(m[0x302], 0xFF) << "sel 没覆盖到的字节被改了";
    EXPECT_EQ(m[0x303], 0xFF);
}

// 8/16 位端口的 ADR 低位语义和 32 位不同：按端口宽度对齐，不能固定折掉低两位
TEST(WbMemory, NarrowPortAlignsByPortWidth) {
    MemFixture f8(16, 8);
    uint8_t *m = f8.bytes();
    m[0x105] = 0x5A;
    m[0x104] = 0xA5;
    uint32_t v = 0;
    ASSERT_TRUE(f8.bus->transact(0x105, false, 0, 0x1, &v));
    EXPECT_EQ(v, 0x5Au) << "8 位端口读 0x105 不能折到 0x104";

    ASSERT_TRUE(f8.bus->transact(0x106, true, 0x77, 0x1));
    EXPECT_EQ(m[0x106], 0x77) << "8 位端口写地址没有按字节寻址";
    EXPECT_EQ(m[0x104], 0xA5) << "写到了错误的地址";

    MemFixture f16(16, 16);
    uint8_t *w = f16.bytes();
    w[0x106] = 0x34;
    w[0x107] = 0x12;
    ASSERT_TRUE(f16.bus->transact(0x106, false, 0, 0x3, &v));
    EXPECT_EQ(v, 0x1234u) << "16 位端口读 0x106 不能折到 0x104";

    ASSERT_TRUE(f16.bus->transact(0x108, true, 0xBEEF, 0x3));
    EXPECT_EQ(w[0x108], 0xEF);
    EXPECT_EQ(w[0x109], 0xBE);
}

// 64 位端口：B4 §1.1 允许数据总线到 64 位，SEL 每字节一位（8 位）
TEST(WbMemory, WideReadWrite64) {
    MemFixture f(16, 64);
    ASSERT_TRUE(f.bus->transact64(0x100, true, 0x1122334455667788ull, 0xFF)) << "64 位写没有 ack";
    uint64_t v = 0;
    ASSERT_TRUE(f.bus->transact64(0x100, false, 0, 0xFF, &v)) << "64 位读没有 ack";
    EXPECT_EQ(v, 0x1122334455667788ull);
    const uint8_t *m = f.bytes();
    EXPECT_EQ(m[0x100], 0x88);
    EXPECT_EQ(m[0x107], 0x11);
}

TEST(WbMemory, PartialWrite64RespectsAllEightSelBits) {
    MemFixture f(16, 64);
    uint8_t *m = f.bytes();
    for (int i = 0; i < 8; ++i) m[0x200 + i] = 0xFF;
    // 只写高 4 个字节
    ASSERT_TRUE(f.bus->transact64(0x200, true, 0xAABBCCDD11223344ull, 0xF0));
    EXPECT_EQ(m[0x200], 0xFF) << "低 4 字节不该被写";
    EXPECT_EQ(m[0x203], 0xFF);
    EXPECT_EQ(m[0x204], 0xDD);
    EXPECT_EQ(m[0x205], 0xCC);
    EXPECT_EQ(m[0x206], 0xBB);
    EXPECT_EQ(m[0x207], 0xAA);
}

TEST(WbMemory, WidePortAlignsToEightBytes) {
    MemFixture f(16, 64);
    uint8_t *m = f.bytes();
    for (int i = 0; i < 8; ++i) m[0x300 + i] = (uint8_t)(0x10 + i);
    uint64_t v = 0;
    // RULE 3.100：ADR 只到端口字边界，低 3 位不参与寻址
    ASSERT_TRUE(f.bus->transact64(0x303, false, 0, 0xFF, &v));
    EXPECT_EQ(v, 0x1716151413121110ull) << "64 位端口读 0x303 不能折到别的字";
    ASSERT_TRUE(f.bus->transact64(0x305, true, 0x8877665544332211ull, 0xFF));
    EXPECT_EQ(m[0x300], 0x11) << "写地址没有按 8 字节字对齐";
    EXPECT_EQ(m[0x307], 0x88);
}

TEST(WbMemory, ConsecutiveWideReadsReturnOwnData) {
    MemFixture f(16, 64);
    uint8_t *m = f.bytes();
    for (int i = 0; i < 16; ++i) m[0x400 + i] = (uint8_t) i;
    uint64_t a = 0, b = 0;
    ASSERT_TRUE(f.bus->transact64(0x400, false, 0, 0xFF, &a));
    ASSERT_TRUE(f.bus->transact64(0x408, false, 0, 0xFF, &b));
    EXPECT_EQ(a, 0x0706050403020100ull);
    EXPECT_EQ(b, 0x0F0E0D0C0B0A0908ull) << "第二笔拿到了上一笔的数据";
}

TEST(WbMemory, WriteThenReadSameAddress) {    MemFixture f;
    ASSERT_TRUE(f.bus->transact(0x400, true, 0x12345678, 0x0F));
    uint32_t v = 0;
    ASSERT_TRUE(f.bus->transact(0x400, false, 0, 0x0F, &v));
    EXPECT_EQ(v, 0x12345678u) << "写后读没拿到刚写的数据";
}

// stb 撤销后 ack 必须跟着撤销，否则主设备会把残留确认当成新事务的结果
TEST(WbMemory, AckDropsWhenStbLow) {
    MemFixture f;
    f.bus->drive(0x100, false, 0, 0x0F, true, true);
    for (int i = 0; i < 20; ++i) {
        f.bus->tick();
        if (f.bus->ackHigh()) break;
    }
    EXPECT_TRUE(f.bus->ackHigh()) << "事务有效期间 ack 应该保持";
    f.bus->drive(0x100, false, 0, 0x0F, false, true);
    f.bus->tick(3);
    EXPECT_FALSE(f.bus->ackHigh()) << "stb 撤销后 ack 还在，会污染下一笔事务";
}

// 精确复现 RISC-V CPU 拆三段事务时驱动总线的波形：
// 每段 stb 拉高直到 ack，段之间 stb 拉低两拍（cyc 保持高）。
// 如果这个能过而 CPU 那边过不了，问题就不在波形上，而在 CPU 自己的状态机
TEST(WbMemory, ThreeSegmentsWithCpuLikeWaveform) {
    MemFixture f;
    uint8_t *m = f.bytes();
    for (int i = 0; i < 12; ++i) m[0x100 + i] = (uint8_t)i;

    uint32_t seg[3] = {0, 0, 0};
    const uint32_t addrs[3] = {0x100, 0x104, 0x108};
    for (int s = 0; s < 3; ++s) {
        f.bus->drive(addrs[s], false, 0, 0x0F, true, true);
        bool got = false;
        for (int i = 0; i < 20; ++i) {
            f.bus->tick();
            if (f.bus->ackHigh()) { got = true; break; }
        }
        ASSERT_TRUE(got) << "第 " << s << " 段没 ack";
        seg[s] = f.bus->readData();
        // 段之间：stb 拉低两拍，cyc 保持高——和 CPU 一样
        f.bus->drive(addrs[s], false, 0, 0x0F, false, true);
        f.bus->tick(2);
    }
    EXPECT_EQ(seg[0], 0x03020100u);
    EXPECT_EQ(seg[1], 0x07060504u);
    EXPECT_EQ(seg[2], 0x0B0A0908u) << "CPU 式波形下第三段拿到了旧数据";
}
