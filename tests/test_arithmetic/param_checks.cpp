// 组合元件的参数范围检查：非法位宽必须在构造期拒绝，不能拖到生成 C 代码
#include <gtest/gtest.h>

#include <dcs/components/combinational/Arithmetic.h>
#include <dcs/components/combinational/FloatPoint.h>
#include <dcs/components/combinational/Reduction.h>
#include <dcs/components/combinational/Shifter.h>

using namespace dsc;

TEST(ParamChecks, Abs) {
    EXPECT_THROW(Abs("a", 0), std::invalid_argument);
    EXPECT_THROW(Abs("a", 65), std::invalid_argument);
    EXPECT_NO_THROW(Abs("a", 1));
    EXPECT_NO_THROW(Abs("a", 64));
}

TEST(ParamChecks, MinMax) {
    EXPECT_THROW(MinMax("m", 0, true, false), std::invalid_argument);
    EXPECT_THROW(MinMax("m", 65, false, true), std::invalid_argument);
    EXPECT_NO_THROW(MinMax("m", 1, true, false));
}

TEST(ParamChecks, Reduction) {
    EXPECT_THROW(Reduction("r", 0, ReductionOp::AND), std::invalid_argument);
    EXPECT_THROW(Reduction("r", 65, ReductionOp::XOR), std::invalid_argument);
    EXPECT_NO_THROW(Reduction("r", 64, ReductionOp::OR));
}

TEST(ParamChecks, Shifter) {
    EXPECT_THROW(Shifter("s", 0, ShiftMode::LSL), std::invalid_argument);
    EXPECT_THROW(Shifter("s", 65, ShiftMode::LSR), std::invalid_argument);
    EXPECT_NO_THROW(Shifter("s", 1, ShiftMode::ASR));
}

TEST(ParamChecks, FloatConversions) {
    EXPECT_THROW(FloatToInt("f", 32, 0, false), std::invalid_argument);
    EXPECT_THROW(FloatToInt("f", 32, 65, false), std::invalid_argument);
    EXPECT_THROW(FloatToInt("f", 16, 8, false), std::invalid_argument) << "非法浮点精度要到构造期拒绝";
    EXPECT_NO_THROW(FloatToInt("f", 64, 32, true));

    EXPECT_THROW(IntToFloat("i", 0, 32, false), std::invalid_argument);
    EXPECT_THROW(IntToFloat("i", 65, 32, false), std::invalid_argument);
    EXPECT_THROW(IntToFloat("i", 8, 16, false), std::invalid_argument);
    EXPECT_NO_THROW(IntToFloat("i", 32, 64, true));
}
