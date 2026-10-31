// tools/verilog2dcs.py 生成的元件：验证生成的包装代码确实驱动了 Verilator 模型
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <gtest/gtest.h>

namespace {

std::unique_ptr<dsc::Component> makeComponent(const std::string &type, const std::string &name) {
    std::string err;
    auto comp = dsc::ComponentFactory::instance().create(type, name, {}, err);
    EXPECT_TRUE(comp) << err;
    return comp;
}

} // namespace

TEST(GeneratedComponent, Adder8Comb) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *cin = c.createNet("cin");
    auto *sum = c.createNet("sum");
    auto *cout = c.createNet("cout");

    auto comp = makeComponent("gen_adder8", "add");
    ASSERT_TRUE(comp);
    auto *add = c.addComponent(std::move(comp));
    c.connect(add, "a", a);
    c.connect(add, "b", b);
    c.connect(add, "cin", cin);
    c.connect(add, "sum", sum);
    c.connect(add, "cout", cout);

    auto err = c.compile();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    c.init();

    c.setWire(a->id(), {200, 0});
    c.setWire(b->id(), {100, 0});
    c.setWire(cin->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 45); // 301 & 0xFF
    EXPECT_EQ(c.getWire(cout->id())[0], 1);

    c.setWire(a->id(), {3, 0});
    c.setWire(b->id(), {5, 0});
    c.setWire(cin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 8);
    EXPECT_EQ(c.getWire(cout->id())[0], 0);
}

TEST(GeneratedComponent, Wide128Ports) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *y = c.createNet("y");

    auto comp = makeComponent("gen_wide128", "w");
    ASSERT_TRUE(comp);
    auto *w = c.addComponent(std::move(comp));
    c.connect(w, "a", a);
    c.connect(w, "y", y);

    auto err = c.compile();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    c.init();

    // 高低 64 位都要搬到位：线网槽是 16 字节，宽端口按整字节拷贝
    std::vector<uint8_t> v(16, 0);
    v[0] = 0x11;
    v[7] = 0x22;
    v[8] = 0x33;
    v[15] = 0x44;
    c.setWire(a->id(), v);
    c.tick();
    EXPECT_EQ(c.getWire(y->id()), v);
}

TEST(GeneratedComponent, IobufTristate) {
    // inout 由 Verilator 的 --pins-inout-enables 拆成 input(pad)+__out+__en，
    // dcs 侧对应 pad_i（外部值）和三态输出 pad。两个实例挂同一条总线：
    // 谁都 oe=1 谁驱动，另一侧高阻，din 读到的永远是总线上的实际值
    dsc::Circuit c;
    auto *bus = c.createNet("bus");

    struct Side {
        dsc::Net *dout;
        dsc::Net *oe;
        dsc::Net *din;
        dsc::Component *comp;
    } sides[2];

    for (int i = 0; i < 2; ++i) {
        sides[i].dout = c.createNet(std::format("dout{}", i));
        sides[i].oe = c.createNet(std::format("oe{}", i));
        sides[i].din = c.createNet(std::format("din{}", i));
        auto comp = makeComponent("gen_iobuf", std::format("io{}", i));
        ASSERT_TRUE(comp);
        sides[i].comp = c.addComponent(std::move(comp));
        c.connect(sides[i].comp, "dout", sides[i].dout);
        c.connect(sides[i].comp, "oe", sides[i].oe);
        c.connect(sides[i].comp, "din", sides[i].din);
        c.connect(sides[i].comp, "pad", bus);
        c.connect(sides[i].comp, "pad_i", bus);
    }

    auto err = c.compile();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    c.init();

    c.setWire(sides[0].dout->id(), {0x5A, 0});
    c.setWire(sides[1].dout->id(), {0x33, 0});
    c.setWire(sides[0].oe->id(), {1, 0});
    c.setWire(sides[1].oe->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id())[0], 0x5A);
    EXPECT_EQ(c.getWire(sides[0].din->id())[0], 0x5A);
    EXPECT_EQ(c.getWire(sides[1].din->id())[0], 0x5A);

    // 交换驱动权
    c.setWire(sides[0].oe->id(), {0, 0});
    c.setWire(sides[1].oe->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id())[0], 0x33);
    EXPECT_EQ(c.getWire(sides[0].din->id())[0], 0x33);
    EXPECT_EQ(c.getWire(sides[1].din->id())[0], 0x33);
}

TEST(GeneratedComponent, Counter8Sequential) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk");
    auto *rst = c.createNet("rst");
    auto *en = c.createNet("en");
    auto *count = c.createNet("count");

    auto comp = makeComponent("gen_counter8", "cnt");
    ASSERT_TRUE(comp);
    auto *cnt = c.addComponent(std::move(comp));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "rst", rst);
    c.connect(cnt, "en", en);
    c.connect(cnt, "count", count);

    auto err = c.compile();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    c.init();

    c.setWire(rst->id(), {0, 0});
    c.setWire(en->id(), {1, 0});

    // 每拍都要驱动 clk：Verilator 模型内部记录上一拍的时钟值来做边沿检测
    for (int i = 1; i <= 3; ++i) {
        c.setWire(clk->id(), {0, 0});
        c.tick();
        c.setWire(clk->id(), {1, 0});
        c.tick();
        EXPECT_EQ(c.getWire(count->id())[0], i);
    }

    // 同步复位：rst 为高的那个上升沿清零
    c.setWire(rst->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(count->id())[0], 0);

    // 使能拉低后不再计数
    c.setWire(rst->id(), {0, 0});
    c.setWire(en->id(), {0, 0});
    for (int i = 0; i < 3; ++i) {
        c.setWire(clk->id(), {0, 0});
        c.tick();
        c.setWire(clk->id(), {1, 0});
        c.tick();
    }
    EXPECT_EQ(c.getWire(count->id())[0], 0);
}
