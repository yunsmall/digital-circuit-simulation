// 蓝图参数：定义级默认值、实例覆盖、${name} 替换、端口位宽表达式
#include <dcs/Circuit.h>
#include <dcs/Context.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

const char *kParamAdder = R"({
  "definitions": {
    "padder": {
      "params": {"bit_width": "4"},
      "nets": ["a", "b", "cin", "sum", "cout"],
      "components": [{
        "type": "adder",
        "name": "u_add",
        "params": {"bit_width": "${bit_width}"},
        "connections": {"a": "a", "b": "b", "cin": "cin", "sum": "sum", "cout": "cout"}
      }],
      "expose": {
        "inputs": {
          "a": {"net": "a", "width": "${bit_width}"},
          "b": {"net": "b", "width": "${bit_width}"},
          "cin": "cin"
        },
        "outputs": {
          "sum": {"net": "sum", "width": "${bit_width}"},
          "cout": "cout"
        }
      }
    }
  },
  "nets": ["a", "b", "cin", "sum", "cout"],
  "components": [{
    "type": "composite",
    "name": "u",
    "params": {"definition": "padder", "bit_width": "8"},
    "connections": {"a": "a", "b": "b", "cin": "cin", "sum": "sum", "cout": "cout"}
  }]
})";

std::vector<uint8_t> wire(uint64_t v) {
    std::vector<uint8_t> b(16, 0);
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t) (v >> (8 * i));
    return b;
}

// 实例 bit_width=8 覆盖定义默认的 4：200+100+1 应回绕成 45，进位 1
TEST(BlueprintParam, InstanceOverridesWidth) {
    dsc::Context ctx;
    std::string error;
    auto c = dsc::Circuit::fromJson(kParamAdder, error, "", &ctx);
    ASSERT_NE(c, nullptr) << error;
    // 展开前复合元件的端口位宽已按参数解析
    ASSERT_EQ(c->components().size(), 1u);
    EXPECT_EQ(c->components()[0]->inputs()[0]->bit_width(), 8);
    for (auto &p: c->components()[0]->outputs())
        if (p->name() == "sum")
            EXPECT_EQ(p->bit_width(), 8);

    ASSERT_EQ(c->compile().code, dsc::ErrorCode::OK);
    c->init();
    c->setWire(c->findNet("a")->id(), wire(200));
    c->setWire(c->findNet("b")->id(), wire(100));
    c->setWire(c->findNet("cin")->id(), wire(1));
    c->tick();
    EXPECT_EQ(c->getWire(c->findNet("sum")->id())[0], 45);
    EXPECT_EQ(c->getWire(c->findNet("cout")->id())[0], 1);
}

// 8 个参数默认位宽：给 8+8+1，4 位和应是 1、进位 1（8 位会是 17、进位 0）
TEST(BlueprintParam, DefinitionDefaultApplies) {
    dsc::Context ctx;
    std::string error;
    std::string json = kParamAdder;
    const std::string override_text = R"(, "bit_width": "8")";
    const size_t pos = json.find(override_text);
    ASSERT_NE(pos, std::string::npos);
    json.erase(pos, override_text.size());

    auto c = dsc::Circuit::fromJson(json, error, "", &ctx);
    ASSERT_NE(c, nullptr) << error;
    ASSERT_EQ(c->components()[0]->inputs()[0]->bit_width(), 4);
    ASSERT_EQ(c->compile().code, dsc::ErrorCode::OK);
    c->init();
    c->setWire(c->findNet("a")->id(), wire(8));
    c->setWire(c->findNet("b")->id(), wire(8));
    c->setWire(c->findNet("cin")->id(), wire(1));
    c->tick();
    EXPECT_EQ(c->getWire(c->findNet("sum")->id())[0], 1);
    EXPECT_EQ(c->getWire(c->findNet("cout")->id())[0], 1);
}

// 定义里没声明的参数必须报错，不能静默忽略
TEST(BlueprintParam, UnknownParamRejected) {
    dsc::Context ctx;
    std::string error;
    std::string json = kParamAdder;
    const std::string from = R"("definition": "padder", "bit_width": "8")";
    const std::string to = R"("definition": "padder", "width": "8")";
    const size_t pos = json.find(from);
    ASSERT_NE(pos, std::string::npos);
    json.replace(pos, from.size(), to);

    auto c = dsc::Circuit::fromJson(json, error, "", &ctx);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(error.find("没有参数"), std::string::npos) << error;
}

// 端口位宽表达式引用未定义参数时也不能带着 ${...} 继续
TEST(BlueprintParam, MissingWidthParamRejected) {
    dsc::Context ctx;
    std::string error;
    std::string json = kParamAdder;
    const std::string from = R"("params": {"bit_width": "4"},)";
    const size_t pos = json.find(from);
    ASSERT_NE(pos, std::string::npos);
    json.replace(pos, from.size(), "");
    const std::string inst = R"(, "bit_width": "8")";
    const size_t ip = json.find(inst);
    ASSERT_NE(ip, std::string::npos);
    json.erase(ip, inst.size());

    auto c = dsc::Circuit::fromJson(json, error, "", &ctx);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(error.find("未定义参数"), std::string::npos) << error;
}

} // namespace
