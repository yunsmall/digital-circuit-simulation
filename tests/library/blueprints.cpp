// library/ 下每个蓝图的行为测试
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include <dcs/Circuit.h>
#include <nlohmann/json.hpp>

using namespace dsc;

namespace {

struct Bp {
    std::unique_ptr<Circuit> c;

    Bp(const char *file, nlohmann::json params, std::initializer_list<const char *> pins) {
        // 单定义蓝图的名字取 $meta.name，没写时取引用键；库里的文件和元数据同名，
        // 这里直接用文件名（去掉 .json）当引用键和 definition 参数
        const std::string def_name = std::filesystem::path(file).stem().string();
        nlohmann::json comp;
        comp["type"] = "composite";
        comp["name"] = "u";
        comp["params"] = std::move(params);
        comp["params"]["definition"] = def_name;
        comp["connections"] = nlohmann::json::object();

        nlohmann::json root;
        root["definitions"][def_name] = std::string(DCS_LIBRARY_DIR) + "/" + file;
        root["nets"] = nlohmann::json::array();
        for (const char *p: pins) {
            root["nets"].push_back(p);
            comp["connections"][p] = p;
        }
        root["components"] = nlohmann::json::array({comp});

        std::string error;
        c = Circuit::fromJson(root.dump(), error, DCS_LIBRARY_DIR);
        if (!c)
            throw std::runtime_error(error);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    void set(const char *net, uint64_t v) {
        auto *n = c->findNet(net);
        if (!n)
            throw std::runtime_error(std::string("没有线网 ") + net);
        c->setWire(n->id(), v);
    }

    uint64_t get(const char *net) const {
        auto *n = c->findNet(net);
        if (!n)
            throw std::runtime_error(std::string("没有线网 ") + net);
        const auto bytes = c->getWire(n->id());
        uint64_t v = 0;
        for (size_t i = 0; i < 8 && i < bytes.size(); i++)
            v |= (uint64_t) bytes[i] << (8 * i);
        return v;
    }

    void tick() { c->tick(); }
};

} // namespace

TEST(LibraryBlueprint, HalfAdder) {
    for (int a = 0; a < 2; a++) {
        for (int b = 0; b < 2; b++) {
            Bp f("half_adder.json", {}, {"a", "b", "sum", "carry"});
            f.set("a", a);
            f.set("b", b);
            f.tick();
            EXPECT_EQ(f.get("sum"), a ^ b) << a << "," << b;
            EXPECT_EQ(f.get("carry"), a & b) << a << "," << b;
        }
    }
}

TEST(LibraryBlueprint, FullAdder) {
    for (int a = 0; a < 2; a++) {
        for (int b = 0; b < 2; b++) {
            for (int cin = 0; cin < 2; cin++) {
                Bp f("full_adder.json", {}, {"a", "b", "cin", "sum", "cout"});
                f.set("a", a);
                f.set("b", b);
                f.set("cin", cin);
                f.tick();
                const int total = a + b + cin;
                EXPECT_EQ(f.get("sum"), total & 1);
                EXPECT_EQ(f.get("cout"), total >> 1);
            }
        }
    }
}

TEST(LibraryBlueprint, ParameterizedAdder) {
    for (int a: {0, 9, 15}) {
        for (int b: {0, 6}) {
            for (int cin: {0, 1}) {
                Bp f("adder.json", {{"bit_width", "4"}}, {"a", "b", "cin", "sum", "cout"});
                f.set("a", a);
                f.set("b", b);
                f.set("cin", cin);
                f.tick();
                const int total = a + b + cin;
                EXPECT_EQ(f.get("sum"), total & 0xF) << a << "+" << b << "+" << cin;
                EXPECT_EQ(f.get("cout"), total >> 4);
            }
        }
    }
}

TEST(LibraryBlueprint, RippleAdder4) {
    Bp f("adder4.json", {}, {"a0", "a1", "a2", "a3", "b0", "b1", "b2", "b3", "cin", "s0", "s1", "s2", "s3",
                              "cout"});
    const int a = 9, b = 6, cin = 1;
    for (int i = 0; i < 4; i++) {
        f.set(("a" + std::to_string(i)).c_str(), (a >> i) & 1);
        f.set(("b" + std::to_string(i)).c_str(), (b >> i) & 1);
    }
    f.set("cin", cin);
    f.tick();
    const int total = a + b + cin;
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(f.get(("s" + std::to_string(i)).c_str()), (total >> i) & 1);
    EXPECT_EQ(f.get("cout"), total >> 4);
}

TEST(LibraryBlueprint, Comparator) {
    struct Case {
        int a, b;
    };
    for (auto [a, b]: {Case{9, 6}, Case{6, 9}, Case{7, 7}}) {
        Bp f("comparator.json", {{"bit_width", "4"}}, {"a", "b", "lt", "eq", "gt"});
        f.set("a", a);
        f.set("b", b);
        f.tick();
        EXPECT_EQ(f.get("lt"), a < b) << a << "," << b;
        EXPECT_EQ(f.get("eq"), a == b) << a << "," << b;
        EXPECT_EQ(f.get("gt"), a > b) << a << "," << b;
    }
}

TEST(LibraryBlueprint, Decoder3To8) {
    for (int sel = 0; sel < 8; sel++) {
        Bp f("decoder3to8.json", {}, {"sel0", "sel1", "sel2", "out0", "out1", "out2", "out3", "out4", "out5", "out6",
                                      "out7"});
        f.set("sel0", sel & 1);
        f.set("sel1", (sel >> 1) & 1);
        f.set("sel2", (sel >> 2) & 1);
        f.tick();
        for (int i = 0; i < 8; i++)
            EXPECT_EQ(f.get(("out" + std::to_string(i)).c_str()), i == sel ? 1u : 0u) << "sel=" << sel;
    }
}

TEST(LibraryBlueprint, Mux4) {
    Bp f("mux4.json", {{"bit_width", "4"}}, {"sel0", "sel1", "in0", "in1", "in2", "in3", "out"});
    f.set("in0", 0x1);
    f.set("in1", 0x2);
    f.set("in2", 0x4);
    f.set("in3", 0x8);
    for (int sel = 0; sel < 4; sel++) {
        f.set("sel0", sel & 1);
        f.set("sel1", (sel >> 1) & 1);
        f.tick();
        EXPECT_EQ(f.get("out"), 1u << sel) << "sel=" << sel;
    }
}

TEST(LibraryBlueprint, Gray4) {
    for (int v = 0; v < 16; v++) {
        Bp f("gray4.json", {}, {"bin", "gray"});
        f.set("bin", v);
        f.tick();
        EXPECT_EQ(f.get("gray"), v ^ (v >> 1)) << "bin=" << v;
    }
}

TEST(LibraryBlueprint, NotGate) {
    for (int v = 0; v < 2; v++) {
        Bp f("not_gate.json", {}, {"in", "out"});
        f.set("in", v);
        f.tick();
        EXPECT_EQ(f.get("out"), v ^ 1);
    }
}

TEST(LibraryBlueprint, ParameterizedParity) {
    struct Case {
        uint64_t din;
        unsigned parity;
    };
    for (auto [din, parity]: {Case{0x00, 0}, Case{0xF1, 1}, Case{0xFF, 0}}) {
        Bp f("parity.json", {{"bit_width", "8"}}, {"din", "parity"});
        f.set("din", din);
        f.tick();
        EXPECT_EQ(f.get("parity"), parity) << "din=" << din;
    }
}

TEST(LibraryBlueprint, PriorityEncoder8) {
    Bp zero("prienc8.json", {}, {"in0", "in1", "in2", "in3", "in4", "in5", "in6", "in7", "out", "valid"});
    zero.tick();
    EXPECT_EQ(zero.get("valid"), 0u);

    for (int i = 0; i < 8; i++) {
        Bp f("prienc8.json", {}, {"in0", "in1", "in2", "in3", "in4", "in5", "in6", "in7", "out", "valid"});
        f.set(("in" + std::to_string(i)).c_str(), 1);
        f.tick();
        EXPECT_EQ(f.get("valid"), 1u) << "in" << i;
        EXPECT_EQ(f.get("out"), (uint64_t) i) << "in" << i;
    }
}
