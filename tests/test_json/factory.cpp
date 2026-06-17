// JSON 序列化：工厂注册类型验证
#include <gtest/gtest.h>
#include <unordered_map>
#include "dcs/ComponentFactory.h"
using namespace dsc;

TEST(JsonRoundTrip, FromFactory) {
    auto &f = ComponentFactory::instance();
    struct {
        const char *factory_name;
        const char *expected_type_name;
        std::unordered_map<std::string, std::string> params;
    } types[] = {
            {"and", "and", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"or", "or", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"nand", "nand", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"nor", "nor", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"xor", "xor", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"xnor", "xnor", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"not", "not", {{"bit_width", "8"}}},
            {"buf", "buf", {{"bit_width", "8"}}},
            {"tsbuf", "tsbuf", {{"bit_width", "8"}}},
            {"dff", "dff", {{"bit_width", "8"}}},
            {"tff", "tff", {{"bit_width", "8"}}},
            {"jkff", "jkff", {{"bit_width", "8"}}},
            {"register", "reg", {{"bit_width", "8"}}},
            {"latch", "latch", {{"bit_width", "8"}}},
            {"counter", "cnt", {{"bit_width", "8"}}},
            {"shift", "sr", {{"length", "8"}}},
            {"mux", "mux", {{"n_selects", "1"}, {"bit_width", "8"}}},
            {"adder", "adder", {{"bit_width", "8"}}},
            {"comparator", "cmp_eq", {{"bit_width", "8"}, {"op", "eq"}}},
            {"decoder", "decoder", {{"n_selects", "2"}}},
            {"constant", "const", {{"bit_width", "8"}, {"value", "42"}}},
            {"switch", "switch", {{"bit_width", "8"}}},
            {"merge", "merge", {{"num_bits", "8"}}},
            {"split", "split", {{"num_bits", "8"}}},
            {"memory", "memory", {{"addr_width", "4"}, {"data_width", "8"}}},
            {"encoder", "encoder", {{"n_selects", "2"}}},
            {"sub", "sub", {{"bit_width", "8"}}},
            {"mul", "mul", {{"bit_width", "8"}}},
            {"rom", "rom", {{"addr_width", "4"}, {"data_width", "8"}}},
    };

    for (auto &t: types) {
        std::string err;
        auto comp = f.create(t.factory_name, "test", t.params, err);
        EXPECT_NE(comp, nullptr) << "无法创建类型: " << t.factory_name;
        if (comp) {
            EXPECT_EQ(comp->typeName(), t.expected_type_name) << "typeName 不匹配: " << t.factory_name;
            auto ep = comp->exportParams();
            EXPECT_FALSE(ep.empty()) << t.factory_name << " 未导出参数";
        }
    }
}

// 参数校验：invalid values 应该被拒绝
TEST(JsonRoundTrip, ParamValidation) {
    auto &f = ComponentFactory::instance();
    std::string err;

    // comparator op 非法
    auto c1 = f.create("comparator", "test1", {{"op", "bad_op"}}, err);
    EXPECT_EQ(c1, nullptr);
    EXPECT_TRUE(err.find("op") != std::string::npos && err.find("bad_op") != std::string::npos);

    // rom source_type 非法
    auto c2 = f.create("rom", "test2", {{"addr_width", "4"}, {"data_width", "8"}, {"source_type", "bad_type"}}, err);
    EXPECT_EQ(c2, nullptr);
    EXPECT_TRUE(err.find("source_type") != std::string::npos);

    // fadd precision 非法
    auto c3 = f.create("fadd", "test3", {{"precision", "80"}}, err);
    EXPECT_EQ(c3, nullptr);
    EXPECT_TRUE(err.find("precision") != std::string::npos && err.find("80") != std::string::npos);

    // 合法值应当成功
    auto c4 = f.create("comparator", "test4", {{"op", "ne"}}, err);
    EXPECT_NE(c4, nullptr);
    EXPECT_EQ(err, "");
}
