// JSON 里后端私有选项的嵌套写法：加载时按键路径拍平成扁平参数键。
//
// 参数表是 string→string 的扁平结构，而 slirp 的 hostfwd、tap 的网卡名这类后端专有
// 配置天然是多键的。约定在 JSON 里写成嵌套对象：
//   "backend_opts": { "hostfwd": "tcp:127.0.0.1:2223-10.0.2.15:23" }
// 加载后变成参数键 "backend_opts.hostfwd"，元件按前缀摘出自己那段
// （见 dcs/net/BackendOpts.h），导出时保持扁平键、重新加载结果不变。
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/platform/VirtioNet.h>
#include <dcs/net/BackendOpts.h>
#include <algorithm>

using namespace dsc;

namespace {

const char *kJson = R"JSON({
  "nets": ["clk"],
  "components": [
    {"type": "virtio_net", "name": "net0",
     "params": {"backend": "none",
                "backend_opts": {"hostfwd": "tcp:127.0.0.1:2223-10.0.2.15:23",
                                 "mtu": 1500,
                                 "nested": {"a": {"b": "deep"}}}},
     "connections": {"clk": "clk"}}
  ]
})JSON";

} // namespace

// 嵌套对象拍平成 a.b 键；标量保持原样（数字也按字符串存）
TEST(JsonNestedParams, FlattensObjects) {
    std::string err;
    auto c = Circuit::fromJson(kJson, err);
    ASSERT_NE(c, nullptr) << err;
    ASSERT_EQ(c->componentCount(), 1u);
    const auto params = c->components()[0]->exportParams();
    EXPECT_EQ(params.at("backend"), "none");
    EXPECT_EQ(params.at("backend_opts.hostfwd"), "tcp:127.0.0.1:2223-10.0.2.15:23");
    EXPECT_EQ(params.at("backend_opts.mtu"), "1500");
    EXPECT_EQ(params.at("backend_opts.nested.a.b"), "deep");
    // 不能留下没拍平的中间层键
    EXPECT_EQ(params.find("backend_opts"), params.end());
    EXPECT_EQ(params.find("backend_opts.nested"), params.end());
}

// 元件按前缀摘选项：只拿自己那段，别的前缀和裸键都不算
TEST(JsonNestedParams, TakeBackendOptsByPrefix) {
    std::unordered_map<std::string, std::string> params = {
            {"backend", "slirp"},
            {"backend_opts.hostfwd", "tcp:2223-23"},
            {"backend_opts.mtu", "1500"},
            {"other_opts.x", "y"},
            {"hostfwd", "裸键不算"},
    };
    const auto opts = takeBackendOpts(params);
    EXPECT_EQ(opts.size(), 2u);
    EXPECT_EQ(opts.at("hostfwd"), "tcp:2223-23");
    EXPECT_EQ(opts.at("mtu"), "1500");
    EXPECT_EQ(opts.find("backend"), opts.end());
    EXPECT_EQ(opts.find("other_opts.x"), opts.end());
}

// 导出再导入：扁平键原样保留，嵌套选项不丢
TEST(JsonNestedParams, RoundTripKeepsOptions) {
    std::string err;
    auto c = Circuit::fromJson(kJson, err);
    ASSERT_NE(c, nullptr) << err;
    const std::string exported = c->exportJson();

    auto c2 = Circuit::fromJson(exported, err);
    ASSERT_NE(c2, nullptr) << err;
    const auto params = c2->components()[0]->exportParams();
    EXPECT_EQ(params.at("backend_opts.hostfwd"), "tcp:127.0.0.1:2223-10.0.2.15:23");
    EXPECT_EQ(params.at("backend_opts.nested.a.b"), "deep");
}

// 元件确实拿到了解析后的选项（不只是躺在参数表里）
TEST(JsonNestedParams, ComponentSeesParsedOptions) {
    std::string err;
    auto c = Circuit::fromJson(kJson, err);
    ASSERT_NE(c, nullptr) << err;
    auto *net = dynamic_cast<VirtioNet *>(c->components()[0].get());
    ASSERT_NE(net, nullptr);
    const auto &opts = net->backendOpts();
    ASSERT_EQ(opts.size(), 3u);
    EXPECT_EQ(opts.at("hostfwd"), "tcp:127.0.0.1:2223-10.0.2.15:23");
    EXPECT_EQ(opts.at("mtu"), "1500");
}
