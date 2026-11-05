// 互连接线告警：从设备端口没接设备，或 stb 接了但 ack 没接回
#include <gtest/gtest.h>

#include <format>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <dcs/components/platform/WishboneInterconnect.h>

using namespace dsc;

namespace {

WishboneInterconnect *addIc(Circuit &c) {
    auto ic = std::make_unique<WishboneInterconnect>("ic0", "0:4096", 1);
    auto *p = ic.get();
    c.addComponent(std::move(ic));
    return p;
}

} // namespace

TEST(WishboneIcTopology, WarnsMissingDevice) {
    Circuit c;
    auto *ic = addIc(c);
    auto msg = ic->checkTopology(c);
    EXPECT_NE(msg.find("没有接到任何设备"), std::string::npos) << msg;
}

// stb 接到别处（这里接一个门当负载）但 ack 没接回：同样是永远等不到应答
TEST(WishboneIcTopology, WarnsMissingAck) {
    Circuit c;
    auto *ic = addIc(c);
    auto g = std::make_unique<LogicGate>("sink", 2, 1, GateOp::AND);
    auto *gp = g.get();
    c.addComponent(std::move(g));
    Net *stb = c.createNet("s0_stb");
    c.connect(ic, "s0_stb_o", stb);
    c.connect(gp, "in0", stb);

    auto msg = ic->checkTopology(c);
    EXPECT_EQ(msg.find("没有接到任何设备"), std::string::npos) << msg;
    EXPECT_NE(msg.find("ack 没有接回"), std::string::npos) << msg;
}

// 段表来自 JSON：数量不设上限时重叠检查是 O(N²)，生成的 else-if 链也会失控
TEST(WishboneIcTopology, RejectsTooManySegments) {
    std::string segs;
    for (int i = 0; i < 257; i++) {
        if (i)
            segs += ",";
        segs += std::format("{}:16", i * 16);
    }
    EXPECT_THROW(WishboneInterconnect("ic", segs, 1), std::invalid_argument);
}
