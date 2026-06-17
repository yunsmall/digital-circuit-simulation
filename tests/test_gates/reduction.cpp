// 归约门测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/Reduction.h>
#include <gtest/gtest.h>

TEST(ReductionAND, AllOnes) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 8, dsc::ReductionOp::AND));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 8'b11111111 → 归约 AND = 1
    std::vector<uint8_t> buf(1, 0xFF);
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}

TEST(ReductionAND, NotAllOnes) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 8, dsc::ReductionOp::AND));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 8'b11110000 → 归约 AND = 0
    std::vector<uint8_t> buf(1, 0xF0);
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
}

TEST(ReductionAND, Wide16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 16, dsc::ReductionOp::AND));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 16'hFFFF → 1
    std::vector<uint8_t> buf(2, 0xFF);
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}

TEST(ReductionOR, AnyOne) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 8, dsc::ReductionOp::OR));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 8'b00100000 → 归约 OR = 1
    std::vector<uint8_t> buf({0x20});
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}

TEST(ReductionOR, AllZero) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 8, dsc::ReductionOp::OR));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 8'h00 → 0
    std::vector<uint8_t> buf(1, 0x00);
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
}

TEST(ReductionXOR, EvenOnes) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 8, dsc::ReductionOp::XOR));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 8'b11110000 → 4 个 1（偶数），XOR = 0
    std::vector<uint8_t> buf(1, 0xF0);
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
}

TEST(ReductionXOR, OddOnes) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 8, dsc::ReductionOp::XOR));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 8'b11100000 → 3 个 1（奇数），XOR = 1
    std::vector<uint8_t> buf(1, 0xE0);
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}

TEST(ReductionXOR, ParityWide32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Reduction>("r", 32, dsc::ReductionOp::XOR));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 32'h0000FFFF → 16 个 1（偶数），XOR = 0
    std::vector<uint8_t> buf(4);
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    c.setWire(in->id(), buf);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
}
