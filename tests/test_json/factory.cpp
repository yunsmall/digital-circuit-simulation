// JSON 序列化：工厂注册类型验证
#include <gtest/gtest.h>
#include <set>
#include <unordered_map>
#include "dcs/Circuit.h"
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
            {"register", "register", {{"bit_width", "8"}}},
            {"latch", "latch", {{"bit_width", "8"}}},
            {"counter", "counter", {{"bit_width", "8"}}},
            {"shift", "shift", {{"length", "8"}}},
            {"mux", "mux", {{"n_selects", "1"}, {"bit_width", "8"}}},
            {"adder", "adder", {{"bit_width", "8"}}},
            {"eq", "eq", {{"bit_width", "8"}}},
            {"ne", "ne", {{"bit_width", "8"}}},
            {"lt", "lt", {{"bit_width", "8"}}},
            {"gt", "gt", {{"bit_width", "8"}}},
            {"le", "le", {{"bit_width", "8"}}},
            {"ge", "ge", {{"bit_width", "8"}}},
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
            {"constant", "constant", {{"bit_width", "8"}, {"value", "42"}}},
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

// 遍历所有已注册类型做 JSON 往返：用默认参数建一个单元件电路，
// 导出再反序列化，检查类型名、构造参数、引脚都没丢。
// 这条测试能一次抓出"setParam 漏参数""typeName 和注册键不一致"这类问题
TEST(JsonRoundTrip, AllRegisteredTypes) {
    auto &f = ComponentFactory::instance();
    // 需要外部资源的类型：dll 要真实 DLL 文件，composite 要先有蓝图定义
    const std::set<std::string> skip = {"dll", "composite"};
    for (auto &[name, meta]: f.metas()) {
        if (skip.count(name))
            continue;
        SCOPED_TRACE("类型: " + name);
        std::string err;
        auto comp = f.create(name, "dev", {}, err);
        ASSERT_NE(comp, nullptr) << err;
        const auto params_before = comp->exportParams();
        const auto inputs_before = comp->inputs().size();
        const auto outputs_before = comp->outputs().size();

        Circuit c;
        Component *cp = c.addComponent(std::move(comp));
        ASSERT_NE(cp, nullptr);
        for (auto &pin: cp->inputs()) {
            Net *n = c.createNet("in_" + name + "_" + pin->name());
            ASSERT_EQ(c.connect(cp, pin->name(), n).code, ErrorCode::OK);
        }
        for (auto &pin: cp->outputs()) {
            Net *n = c.createNet("out_" + name + "_" + pin->name());
            ASSERT_EQ(c.connect(cp, pin->name(), n).code, ErrorCode::OK);
        }

        const std::string json = c.exportJson();
        std::string error;
        auto c2 = Circuit::fromJson(json, error);
        ASSERT_NE(c2, nullptr) << error;
        ASSERT_EQ(c2->componentCount(), 1);
        auto *cp2 = c2->componentById(0);
        EXPECT_EQ(cp2->typeName(), meta.type_name);
        EXPECT_EQ(cp2->exportParams(), params_before);
        EXPECT_EQ(cp2->inputs().size(), inputs_before);
        EXPECT_EQ(cp2->outputs().size(), outputs_before);
    }
}

