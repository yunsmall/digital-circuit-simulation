// 真值表 / 卡诺图导出：把组合电路的输入网全组合枚举一遍，读输出网。
//
// 只处理单 bit 输入、单 bit 输出的组合电路。输入网必须是外部输入（没被输出引脚
// 驱动），否则写进去的值会在 tick 时被电路自己覆盖。
#pragma once
#include <string>
#include <vector>

namespace dsc {

class Circuit;

struct TruthTable {
    std::vector<std::string> inputs; // 变量名，inputs[0] 是二进制组合的最高位
    std::vector<uint8_t> values;     // 长度 2^n，下标就是输入组合

    int vars() const { return (int) inputs.size(); }
};

// 枚举全部组合。每个组合设置输入后 tick 到稳定再读输出。
// 失败时返回 false 并写 error（网不存在、位宽不是 1、变量太多等）。
bool computeTruthTable(Circuit &c, const std::vector<std::string> &inputs, const std::string &output, TruthTable &out,
                       std::string &error);

std::string formatTruthTable(const TruthTable &t);

// 2~6 变量输出卡诺图（行 = 前 n/2 个变量，列 = 其余变量，格雷码排列）；
// 变量太多时返回一句说明而不是图
std::string formatKarnaugh(const TruthTable &t);

} // namespace dsc
