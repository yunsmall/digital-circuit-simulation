// JIT 符号表是"按名字索引、后注册覆盖先注册"的，两个元件把同一个名字注册成不同
// 地址时，生成代码会调到另一家的回调并拿对方的状态指针来解释。真实踩过：五个
// virtio 设备都导出 dcs_virtio_reset，最后注册的 thunk 顶替了其它设备，
// guest 一复位 virtio-input 就按 virtio-net 的 State 布局读函数指针，宿主进程崩掉。
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <dcs/components/platform/VirtioInput.h>
#include <dcs/components/platform/VirtioRng.h>

#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace dsc;

namespace {

// 只用来导出符号的最小组合元件：输出恒为 0
class SymbolSource : public CombinationalComponent {
public:
    SymbolSource(const std::string &name, std::string sym_name, void *sym) :
        CombinationalComponent(name, "symbol_source"), _sym_name(std::move(sym_name)), _sym(sym) {
        addInput("in", 8);
        addOutput("out", 8);
    }

    std::string genFuncDef_comb() const override {
        return std::format("static void {}() {{\n    const uint8_t _o = 0;\n{}\n}}\n", funcName_comb(),
                           genOutputWrite(0, "_o", 8));
    }

    std::unique_ptr<Component> clone(const std::string &n) const override {
        return std::make_unique<SymbolSource>(n, _sym_name, _sym);
    }

    std::vector<JitSymbol> extraJitSymbols() const override {
        return {{_sym_name, _sym}};
    }

private:
    std::string _sym_name;
    void *_sym;
};

// 两个元件、一条线网的最小电路：源元件输出到同一个出线网（多驱动在 check 之前不查，
// 这里本来就是编译期符号检查，不需要真的能跑）
void buildTwoSources(Circuit &c, void *a, void *b, const std::string &sym_a, const std::string &sym_b) {
    auto *in0 = c.createNet("in0");
    auto *out = c.createNet("out");
    auto *s0 = c.addComponent(std::make_unique<SymbolSource>("s0", sym_a, a));
    auto *s1 = c.addComponent(std::make_unique<SymbolSource>("s1", sym_b, b));
    c.connect(s0, "in", in0);
    c.connect(s1, "in", in0);
    c.connect(s0, "out", out);
    c.connect(s1, "out", out);
}

} // namespace

// 同名不同址：必须在编译期报出来，不能悄悄留下一个
TEST(JitSymbols, DuplicateNameDifferentAddressFails) {
    Circuit c;
    buildTwoSources(c, reinterpret_cast<void *>(0x1000), reinterpret_cast<void *>(0x2000), "dcs_test_sym", "dcs_test_sym");
    const auto e = c.compile();
    EXPECT_EQ(e.code, ErrorCode::JIT_SYMBOL_FAILED);
    EXPECT_NE(e.message.find("dcs_test_sym"), std::string::npos) << e.message;
    EXPECT_NE(e.message.find("冲突"), std::string::npos) << e.message;
}

// 同名同址是正常的：多个元件可以导出同一个全局函数
TEST(JitSymbols, DuplicateNameSameAddressIsFine) {
    Circuit c;
    auto *same = reinterpret_cast<void *>(0x1000);
    buildTwoSources(c, same, same, "dcs_test_sym", "dcs_test_sym");
    EXPECT_EQ(c.compile().code, ErrorCode::OK);
}

// 不同名各自导出互不影响
TEST(JitSymbols, DistinctNamesAreFine) {
    Circuit c;
    buildTwoSources(c, reinterpret_cast<void *>(0x1000), reinterpret_cast<void *>(0x2000), "dcs_test_a", "dcs_test_b");
    EXPECT_EQ(c.compile().code, ErrorCode::OK);
}

// 回归：不同 virtio 设备的复位回调必须按 id 区分，否则共用名字会在运行期串号
TEST(JitSymbols, VirtioResetSymbolsArePerComponent) {
    Circuit c;
    auto *a = c.addComponent(std::make_unique<VirtioInput>("vin"));
    auto *b = c.addComponent(std::make_unique<VirtioRng>("vrng"));
    auto resetName = [](const Component *comp) {
        for (auto &[name, ptr]: comp->extraJitSymbols())
            if (name.rfind("dcs_virtio_", 0) == 0 && name.find("_reset") != std::string::npos)
                return name;
        return std::string{};
    };
    const std::string na = resetName(a), nb = resetName(b);
    EXPECT_FALSE(na.empty());
    EXPECT_FALSE(nb.empty());
    EXPECT_NE(na, nb);
}
