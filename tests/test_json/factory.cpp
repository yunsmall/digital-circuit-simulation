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
            {"eq", "cmp_eq", {{"bit_width", "8"}}},
            {"ne", "cmp_ne", {{"bit_width", "8"}}},
            {"lt", "cmp_lt", {{"bit_width", "8"}}},
            {"gt", "cmp_gt", {{"bit_width", "8"}}},
            {"le", "cmp_le", {{"bit_width", "8"}}},
            {"ge", "cmp_ge", {{"bit_width", "8"}}},
            {"edgedet_rise", "edgedet_rise", {}},
            {"edgedet_fall", "edgedet_fall", {}},
            {"edgedet_both", "edgedet_both", {}},
            {"redand", "redand", {{"bit_width", "8"}}},
            {"redor", "redor", {{"bit_width", "8"}}},
            {"redxor", "redxor", {{"bit_width", "8"}}},
            {"lsl", "lsl", {{"bit_width", "8"}}},
            {"lsr", "lsr", {{"bit_width", "8"}}},
            {"asr", "asr", {{"bit_width", "8"}}},
            {"min", "min", {{"bit_width", "8"}}},
            {"max", "max", {{"bit_width", "8"}}},
            {"abs", "abs", {{"bit_width", "8"}}},
            {"fadd", "fadd", {{"precision", "32"}}},
            {"fsub", "fsub", {{"precision", "32"}}},
            {"fmul", "fmul", {{"precision", "32"}}},
            {"fdiv", "fdiv", {{"precision", "32"}}},
            {"feq", "feq", {{"precision", "32"}}},
            {"fne", "fne", {{"precision", "32"}}},
            {"flt", "flt", {{"precision", "32"}}},
            {"fgt", "fgt", {{"precision", "32"}}},
            {"fle", "fle", {{"precision", "32"}}},
            {"fge", "fge", {{"precision", "32"}}},
            {"f2i", "f2i", {{"fp", "32"}, {"int_width", "8"}}},
            {"i2f", "i2f", {{"int_width", "8"}, {"fp", "32"}}},
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
            // 无参数元件（如 EdgeDetect）exportParams() 返回空是合法的
        }
    }
}

// 参数校验：invalid values 应该被拒绝
TEST(JsonRoundTrip, ParamValidation) {
    auto &f = ComponentFactory::instance();
    std::string err;

    // rom source_type 非法
    auto c1 = f.create("rom", "test1", {{"addr_width", "4"}, {"data_width", "8"}, {"source_type", "bad_type"}}, err);
    EXPECT_EQ(c1, nullptr);
    EXPECT_TRUE(err.find("source_type") != std::string::npos);

    // fadd precision 非法
    auto c2 = f.create("fadd", "test2", {{"precision", "80"}}, err);
    EXPECT_EQ(c2, nullptr);
    EXPECT_TRUE(err.find("precision") != std::string::npos && err.find("80") != std::string::npos);

    // 合法值应当成功
    auto c3 = f.create("ne", "test3", {{"bit_width", "8"}}, err);
    EXPECT_NE(c3, nullptr);
    EXPECT_EQ(err, "");
}
