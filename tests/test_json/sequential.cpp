// JSON 序列化：时序电路
#include "dcs/components/Sequential.h"
#include <gtest/gtest.h>
#include "dcs/Circuit.h"
using namespace dsc;

TEST(JsonRoundTrip, Sequential) {
    Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *dff = c.addComponent(std::make_unique<DFlipFlop>("dff1", 8));
    c.connect(dff, "d", d);
    c.connect(dff, "clk", clk);
    c.connect(dff, "q", q);

    std::string json = c.exportJson();
    std::string error;
    auto c2 = Circuit::fromJson(json, error);
    ASSERT_TRUE(c2) << error;
    EXPECT_EQ(c2->netCount(), 3);
    EXPECT_EQ(c2->componentCount(), 1);
    EXPECT_EQ(c2->componentById(0)->typeName(), "dff");
}
