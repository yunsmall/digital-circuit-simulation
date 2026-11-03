// 65~128 位线网：线网槽本来就是 16 字节，宽线网按整字节搬运
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <gtest/gtest.h>

namespace {

// lo 放在低 8 字节，hi 放在高 8 字节（线网内存布局是小端）
std::vector<uint8_t> bytes128(uint64_t lo, uint64_t hi) {
    std::vector<uint8_t> v(16, 0);
    std::memcpy(v.data(), &lo, 8);
    std::memcpy(v.data() + 8, &hi, 8);
    return v;
}

} // namespace

TEST(Width128, BufPassThrough) {
    dsc::Circuit c;
    auto *in = c.createNet("in");
    auto *out = c.createNet("out");
    auto *buf = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 128, false));
    c.connect(buf, "in", in);
    c.connect(buf, "out", out);

    auto err = c.compile();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    c.init();

    auto v = bytes128(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL);
    c.setWire(in->id(), v);
    c.tick();
    EXPECT_EQ(c.getWire(out->id()), v);

    // 只翻高 64 位也要跟得上
    v = bytes128(0, 0xFFFFFFFFFFFFFFFFULL);
    c.setWire(in->id(), v);
    c.tick();
    EXPECT_EQ(c.getWire(out->id()), v);
}

TEST(Width128, TsBufDrivesBus) {
    dsc::Circuit c;
    auto *in = c.createNet("in");
    auto *oe = c.createNet("oe");
    auto *bus = c.createNet("bus");
    auto *ts = c.addComponent(std::make_unique<dsc::GateTSBUF>("ts", 128));
    c.connect(ts, "in", in);
    c.connect(ts, "oe", oe);
    c.connect(ts, "out", bus);

    auto err = c.compile();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    c.init();

    auto v = bytes128(0xDEADBEEFCAFEBABEULL, 0x0011223344556677ULL);
    c.setWire(in->id(), v);
    c.setWire(oe->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id()), v);
}

// >64 位且不是 8 的倍数的引脚：按字节拷贝后最后一个字节里超出的位是线网值，
// 必须按字节掩掉，否则元件会读到线网高位的脏数据
TEST(Width128, NonByteAlignedReadMasksTailBits) {
    dsc::Circuit c;
    auto *in = c.createNet("in");
    auto *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 100, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);

    auto err = c.validate();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    const std::string code = c.generateC();
    // 100 位 = 13 字节，第 13 个字节只有低 4 位有效
    EXPECT_NE(code.find("((uint8_t *)&_in)[12] &= 0x0F;"), std::string::npos)
            << "100 位输入没有清掉最后一个字节的高 4 位";
}
