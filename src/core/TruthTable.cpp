#include "dcs/TruthTable.h"

#include <format>
#include <sstream>

#include "dcs/Circuit.h"

namespace dsc {

bool computeTruthTable(Circuit &c, const std::vector<std::string> &inputs, const std::string &output, TruthTable &out,
                       std::string &error) {
    if (inputs.empty()) {
        error = "至少要指定一个输入网";
        return false;
    }
    if (inputs.size() > 16) {
        error = "输入变量太多（最多 16 个）";
        return false;
    }
    std::vector<Net *> in_nets;
    for (const auto &name: inputs) {
        Net *n = c.findNet(name);
        if (!n) {
            error = std::format("找不到输入网 '{}'", name);
            return false;
        }
        if (n->bit_width() != 1) {
            error = std::format("输入网 '{}' 位宽是 {}，真值表只支持 1 位", name, n->bit_width());
            return false;
        }
        in_nets.push_back(n);
    }
    Net *out_net = c.findNet(output);
    if (!out_net) {
        error = std::format("找不到输出网 '{}'", output);
        return false;
    }
    if (out_net->bit_width() != 1) {
        error = std::format("输出网 '{}' 位宽是 {}，真值表只支持 1 位", output, out_net->bit_width());
        return false;
    }

    out.inputs = inputs;
    const int n = (int) inputs.size();
    const uint32_t total = 1u << n;
    out.values.assign(total, 0);

    c.init();
    // 组合逻辑在一个 tick 内就传播到底（拓扑序），多走几拍是为了兼容长链/三态
    constexpr int SETTLE = 4;
    for (uint32_t combo = 0; combo < total; combo++) {
        for (int k = 0; k < n; k++) {
            const uint8_t bit = (uint8_t) ((combo >> (n - 1 - k)) & 1u);
            c.setWire(in_nets[k]->id(), {bit});
        }
        for (int i = 0; i < SETTLE; i++)
            c.tick();
        auto v = c.getWire(out_net->id());
        out.values[combo] = v.empty() ? 0 : (uint8_t) (v[0] & 1u);
    }
    return true;
}

std::string formatTruthTable(const TruthTable &t) {
    std::ostringstream os;
    for (const auto &name: t.inputs)
        os << name << " ";
    os << "| y\n";
    for (size_t i = 0; i < t.inputs.size(); i++)
        os << "--";
    os << "+--\n";
    const int n = t.vars();
    for (uint32_t combo = 0; combo < t.values.size(); combo++) {
        for (int k = 0; k < n; k++)
            os << ((combo >> (n - 1 - k)) & 1u) << " ";
        os << "| " << (int) t.values[combo] << "\n";
    }
    return os.str();
}

namespace {
// 格雷码序列：第 i 个标签 = i ^ (i >> 1)
uint32_t grayAt(uint32_t i) {
    return i ^ (i >> 1);
}

std::string grayLabel(uint32_t g, int bits) {
    std::string s;
    for (int k = bits - 1; k >= 0; k--)
        s += (char) ('0' + ((g >> k) & 1u));
    return s;
}
} // namespace

std::string formatKarnaugh(const TruthTable &t) {
    const int n = t.vars();
    if (n < 2 || n > 6)
        return std::format("（{} 变量：卡诺图只导出 2~6 变量，其他情况看真值表）\n", n);

    const int row_bits = n <= 3 ? 1 : n / 2;
    const int col_bits = n - row_bits;
    const uint32_t rows = 1u << row_bits;
    const uint32_t cols = 1u << col_bits;

    std::ostringstream os;
    // 列变量名
    os << "        ";
    for (int k = row_bits; k < n; k++)
        os << t.inputs[k];
    os << "\n";
    // 格雷码列标签
    os << "        ";
    for (uint32_t j = 0; j < cols; j++)
        os << std::format("{:>4}", grayLabel(grayAt(j), col_bits));
    os << "\n";
    for (uint32_t i = 0; i < rows; i++) {
        const uint32_t g = grayAt(i);
        // 行标签写成"变量=值"的形式，比如 a=1 或 ab=01
        std::string name;
        for (int k = 0; k < row_bits; k++)
            name += t.inputs[k];
        os << std::format("{:>6}  ", name + "=" + grayLabel(g, row_bits));
        for (uint32_t j = 0; j < cols; j++) {
            const uint32_t combo = (g << col_bits) | grayAt(j);
            os << std::format("{:>4}", (int) t.values[combo]);
        }
        os << "\n";
    }
    return os.str();
}

} // namespace dsc
