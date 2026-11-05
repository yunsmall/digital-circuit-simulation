// JSON 序列化：工厂注册类型验证
#include <gtest/gtest.h>
#include <set>
#include <unordered_map>
#include "dcs/Circuit.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/VirtioBlkMem.h"
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


// 第三方引入了这个库以后：可以直接用自己的字符串分类，不需要改库里的枚举；
// 没登记过的分类按 key 原样显示
TEST(ComponentFactory, CustomCategory) {
    auto &f = dsc::ComponentFactory::instance();
    f.registerCategory("audio", "音频");
    EXPECT_EQ(f.categoryDisplayName("audio"), "音频");
    EXPECT_EQ(f.categoryDisplayName("not_registered"), "not_registered");

    // 字符串分类
    dsc::ComponentFactory::Meta custom{"my_comp", {}, {}, {}, std::string("audio"), "第三方元件"};
    EXPECT_EQ(custom.category, "audio");

    // 内置枚举仍然照旧
    dsc::ComponentFactory::Meta builtin{"builtin_comp", {}, {}, {}, dsc::ComponentCategory::Gate, ""};
    EXPECT_EQ(builtin.category, "gate");
    EXPECT_EQ(f.categoryDisplayName(builtin.category), "门电路");
}

// Meta 的 valid_values 必须和构造函数实际支持的范围一致。
// wishbone_mem 的引脚只有 32 位，64 位数据会在构造时被拒绝
TEST(JsonRoundTrip, MemoryDataWidthMatchesDeclaration) {
    auto &f = ComponentFactory::instance();
    std::string err;
    auto c = f.create("wishbone_mem", "m", {{"data_width", "64"}}, err);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(err.find("data_width"), std::string::npos)
        << "应该由 Meta 的参数校验拦下，而不是构造后才报位宽错误: " << err;
}

// paramInt 解析失败会回退默认值，加载期必须在 validateParams 拦住，
// 否则 "bit_width": "abc" 会静默按默认位宽跑
TEST(JsonRoundTrip, NonNumericIntParamIsRejected) {
    auto &f = ComponentFactory::instance();
    std::string err;
    auto c = f.create("and", "g", {{"inputs", "2"}, {"bit_width", "abc"}}, err);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(err.find("整数"), std::string::npos) << err;
}

// typeName 必须是工厂注册键：有符号乘/除原来导出成 smul/sdiv，回读直接找不到类型
TEST(JsonRoundTrip, SignedMulDivRoundTrip) {
    auto &f = ComponentFactory::instance();
    for (const char *type: {"mul", "div"}) {
        std::string err;
        auto c = f.create(type, "dut", {{"bit_width", "8"}, {"signed", "1"}}, err);
        ASSERT_NE(c, nullptr) << err;
        EXPECT_EQ(c->typeName(), type);
        EXPECT_EQ(c->exportParams()["signed"], "1");
        auto again = f.create(c->typeName(), "dut2", c->exportParams(), err);
        EXPECT_NE(again, nullptr) << "导出后回读失败: " << err;
        EXPECT_EQ(again->exportParams()["signed"], "1");
    }
}

// 有符号加法器的 cout 语义（溢出 vs 进位）必须随 JSON 往返保留
TEST(JsonRoundTrip, SignedAdderRoundTrip) {
    auto &f = ComponentFactory::instance();
    std::string err;
    auto c = f.create("adder", "a", {{"bit_width", "8"}, {"signed", "1"}}, err);
    ASSERT_NE(c, nullptr) << err;
    EXPECT_EQ(c->exportParams()["signed"], "1");
    auto again = f.create(c->typeName(), "a2", c->exportParams(), err);
    ASSERT_NE(again, nullptr) << err;
    EXPECT_EQ(again->exportParams()["signed"], "1");
}

// xlen 不导出的话，RV64 电路导出再导入会静默变成 RV32
TEST(JsonRoundTrip, CpuXlenIsExported) {
    auto &f = ComponentFactory::instance();
    std::string err;
    auto c = f.create("riscv_cpu", "cpu", {{"xlen", "64"}}, err);
    ASSERT_NE(c, nullptr) << err;
    ASSERT_EQ(c->exportParams().count("xlen"), 1u);
    EXPECT_EQ(c->exportParams()["xlen"], "64");
    auto again = f.create("riscv_cpu", "cpu2", c->exportParams(), err);
    ASSERT_NE(again, nullptr) << err;
    EXPECT_EQ(again->exportParams()["xlen"], "64");
}

// Constant 的 value 支持十六进制，加载期的整数校验不能用十进制规则拒绝它
TEST(JsonRoundTrip, ConstantHexValue) {
    auto &f = ComponentFactory::instance();
    std::string err;
    auto c = f.create("constant", "k", {{"bit_width", "16"}, {"value", "0x10"}}, err);
    ASSERT_NE(c, nullptr) << err;
    EXPECT_EQ(c->exportParams()["value"], "16");
}

// 延迟直接是生成代码里静态数组的维度，必须有上界
TEST(JsonRoundTrip, LatencyUpperBound) {
    auto &f = ComponentFactory::instance();
    std::string err;
    EXPECT_EQ(f.create("memory", "m", {{"addr_width", "4"}, {"data_width", "8"}, {"read_latency", "100000"}}, err),
              nullptr);
    EXPECT_EQ(f.create("mpram", "r", {{"addr_width", "4"}, {"data_width", "8"}, {"read_latency", "100000"}}, err),
              nullptr);
}

// base + size 越过 4 GiB 会让 32 位回绕的段判断误命中
TEST(JsonRoundTrip, DcacheSegmentOverflowRejected) {
    auto &f = ComponentFactory::instance();
    std::string err;
    auto c = f.create("wb_dcache", "d", {{"lines", "16"}, {"base", "4294963200"}, {"size", "8192"}}, err);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(err.find("32 位"), std::string::npos) << err;
}

// ROM / ROM 适配器的读延迟同样进入生成代码的数组维度或等待拍数
TEST(JsonRoundTrip, RomLatencyUpperBound) {
    auto &f = ComponentFactory::instance();
    std::string err;
    EXPECT_EQ(f.create("rom", "r", {{"addr_width", "4"}, {"data_width", "8"}, {"read_latency", "-1"}}, err), nullptr);
    EXPECT_EQ(f.create("rom", "r", {{"addr_width", "4"}, {"data_width", "8"}, {"read_latency", "100000"}}, err),
              nullptr);
    EXPECT_EQ(f.create("wishbone_rom_adapter", "a",
                       {{"addr_width", "4"}, {"data_width", "8"}, {"read_latency", "-5"}}, err),
              nullptr);
}

// size_kb 向下取整会把 1536 字节导成 1 KB，回读后容量缩水；size_bytes 必须精确往返
TEST(JsonRoundTrip, VirtioBlkMemSizeBytesRoundTrip) {
    auto &f = ComponentFactory::instance();
    std::string err;
    dsc::VirtioBlkMem dev("vd", 1536);
    ASSERT_EQ(dev.memSize(), 1536u);
    auto again = f.create("virtio_blk_mem", "vd2", dev.exportParams(), err);
    ASSERT_NE(again, nullptr) << err;
    EXPECT_EQ(static_cast<dsc::VirtioBlkMem *>(again.get())->memSize(), 1536u);
}

// 小于扇区大小的容量对齐后会变成 0，必须在工厂里拒绝
TEST(JsonRoundTrip, VirtioBlkMemRejectsSubSectorSize) {
    auto &f = ComponentFactory::instance();
    std::string err;
    EXPECT_EQ(f.create("virtio_blk_mem", "vd", {{"size_bytes", "511"}}, err), nullptr);
    EXPECT_NE(err.find("512"), std::string::npos) << err;
}

// 可选引脚参数必须能从 JSON 表达，否则 --pins 和反序列化都拿不到 en/rst/pre
TEST(JsonRoundTrip, DffOptionalPinsFromJson) {
    auto &f = ComponentFactory::instance();
    std::string err;
    auto dff = f.create("dff", "d", {{"bit_width", "8"}, {"has_en", "1"}, {"has_rst", "1"}, {"has_preset", "1"}}, err);
    ASSERT_NE(dff, nullptr) << err;
    EXPECT_EQ(dff->inputs().size(), 5u); // d clk en rst pre
    EXPECT_EQ(dff->outputs().size(), 1u);

    auto reg = f.create("register", "r", {{"bit_width", "8"}, {"has_en", "1"}, {"has_rst", "1"}}, err);
    ASSERT_NE(reg, nullptr) << err;
    EXPECT_EQ(reg->inputs().size(), 4u); // d clk en rst
}
