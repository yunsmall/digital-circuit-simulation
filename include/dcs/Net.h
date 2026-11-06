#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace dsc {

// 输入引脚的边沿/变化依赖。对应 Verilog 的 always @(posedge/negedge/信号)。
// Rising/Falling 只对 1 位信号有意义；Change 等价于 always @(a)，
// 任意位宽都可以声明，任一位变化都算一次触发。
enum class PinEdge { None, Rising, Falling, Change };

// ============================================================
// Net — 电路中的一根导线（线网）
//
// 位宽规则：由所有连接到该线的引脚中的最高位宽决定
//   - 初始为 0（未确定）
//   - 每次引脚连接时自动更新为 max(线网位宽, 引脚位宽)
//   - 当引脚位宽 < 线网位宽时，该引脚只读写线的低 N 位
//
// 驱动规则：
//   - 一根线网只能被一个输出引脚驱动（Circuit::connect 强制检查）
//   - 可被任意多个输入引脚读取
//   - 悬空输入读取值为 0
//
// 在 JIT 编译的 C 代码中对应: uint8_t _w[id][16]（16 字节，最大支持 128 位）
// ============================================================
class Net {
public:
    // id: 线网在电路中的唯一索引，对应 C 数组下标 _w[id]
    Net(const std::string &name, int id);

    const std::string &name() const {
        return _name;
    }
    int id() const {
        return _id;
    }
    // 只由 Circuit 在删除线网后调用：生成的代码用 id 作 _w[] 下标，必须保持连续
    void setId(int id) {
        _id = id;
    }
    int bit_width() const {
        return _bit_width;
    }
    // 伪时钟线：clkgen 声明"每 tick 都算上升/下降沿"，边沿检测直接返回真，
    // 不生成比较代码。只能是一个方向，None 表示普通线网
    PinEdge pseudoEdge() const {
        return _pseudo_edge;
    }
    void setPseudoEdge(PinEdge e) {
        _pseudo_edge = e;
    }

    // 连接引脚时调用，位宽取两者较大值
    void updateBitWidth(int pw) {
        _bit_width = std::max(_bit_width, pw);
    }
    // 只由 Circuit 在断开/删除后重算位宽时调用：位宽必须能随连接减少而回退，
    // 否则改接更窄的引脚后生成代码仍按旧宽度处理
    void resetBitWidth() {
        _bit_width = 0;
    }

private:
    std::string _name;
    int _id;
    int _bit_width = 0;
    PinEdge _pseudo_edge = PinEdge::None;
};

} // namespace dsc
