// Verilator 集成测试：adder8.v → DLL → DllComponent
#include <dcs/Circuit.h>
#include <dcs/components/Dll.h>
#include <gtest/gtest.h>

static const char *kDllPath = "adder8_dll.dll";

TEST(VerilatorAdder8, BasicAdd) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *cin = c.createNet("cin");
    auto *sum = c.createNet("sum");
    auto *cout = c.createNet("cout");

    auto *dll = c.addComponent(std::make_unique<dsc::DllComponent>("adder", kDllPath));
    c.connect(dll, "a", a);
    c.connect(dll, "b", b);
    c.connect(dll, "cin", cin);
    c.connect(dll, "sum", sum);
    c.connect(dll, "cout", cout);

    auto err = c.compile();
    ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    c.init();

    // 3 + 5 = 8, cout=0
    c.setWire(a->id(), {3, 0});
    c.setWire(b->id(), {5, 0});
    c.setWire(cin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 8);
    EXPECT_EQ(c.getWire(cout->id())[0], 0);

    // 255 + 1 = 0 (溢出), cout=1
    c.setWire(a->id(), {255, 0});
    c.setWire(b->id(), {1, 0});
    c.setWire(cin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0);
    EXPECT_EQ(c.getWire(cout->id())[0], 1);

    // 128 + 128 = 0 (不带进位), cout=1
    c.deinit();
}

TEST(VerilatorAdder8, WithCarryIn) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *cin = c.createNet("cin");
    auto *sum = c.createNet("sum");
    auto *cout = c.createNet("cout");

    auto *dll = c.addComponent(std::make_unique<dsc::DllComponent>("adder", kDllPath));
    c.connect(dll, "a", a);
    c.connect(dll, "b", b);
    c.connect(dll, "cin", cin);
    c.connect(dll, "sum", sum);
    c.connect(dll, "cout", cout);

    c.compile();
    c.init();

    // 3 + 5 + cin=1 = 9
    c.setWire(a->id(), {3, 0});
    c.setWire(b->id(), {5, 0});
    c.setWire(cin->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 9);
    EXPECT_EQ(c.getWire(cout->id())[0], 0);
}
