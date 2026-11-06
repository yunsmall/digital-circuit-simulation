#include "dcs/TruthTable.h"

#include <format>
#include <sstream>
#include <unordered_set>

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
    std::unordered_set<std::string> seen;
    for (const auto &name: inputs) {
        // 同一个网出现两次时，枚举里后一次赋值会覆盖前一次，表只反映最后一次写入
        if (!seen.insert(name).second) {
            error = std::format("输入网 '{}' 重复", name);
            return false;
        }
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

    // 真值表靠从外部写输入网来枚举组合。输入网如果被元件的输出引脚驱动，写进去的
    // 值会在 tick 里被电路自己覆盖，枚举出来的表是错的，所以在入口直接拒绝
    for (Net *n: in_nets) {
        for (auto &comp: c.components()) {
            for (auto &out: comp->outputs()) {
                if (out->net() == n) {
                    error = std::format("输入网 '{}' 被元件 '{}' 的输出引脚 '{}' 驱动，不能作为真值表输入",
                                        n->name(), comp->name(), out->name());
                    return false;
                }
            }
        }
    }

    // 真值表靠反复写输入网并 settle 若干拍来枚举组合关系。电路里有任何时序元件，
    // tick 都会推进它的状态，枚举出来的就是一条带状态的轨迹，而不是真值表
    for (auto &comp: c.components()) {
        if (comp->hasSequential()) {
            error = std::format("真值表只支持纯组合电路，元件 '{}' 是时序元件", comp->name());
            return false;
        }
    }

    out.inputs = inputs;
    const int n = (int) inputs.size();
    const uint32_t total = 1u << n;
    out.values.assign(total, 0);

    c.init();
    // init 失败时 tick 会直接返回，后面读到的是线网初值，真值表会静默全 0
    if (c.hasError()) {
        error = c.tickError().message;
        return false;
    }
    // 组合逻辑在一个 tick 内就传播到底（拓扑序），多走几拍是为了兼容长链/三态
    constexpr int SETTLE = 4;
    for (uint32_t combo = 0; combo < total; combo++) {
        for (int k = 0; k < n; k++) {
            const uint8_t bit = (uint8_t) ((combo >> (n - 1 - k)) & 1u);
            c.setWire(in_nets[k]->id(), bit);
        }
        for (int i = 0; i < SETTLE; i++)
            c.tick();
        // 运行期错误（三态反馈环振荡、总线冲突）后 tick 会变成 no-op，
        // 继续枚举只会把"停住时的残值"当成真值表输出
        if (c.hasError()) {
            error = c.tickError().message;
            return false;
        }
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
