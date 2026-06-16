#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace dsc {

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
// 在 JIT 编译的 C 代码中对应: uint8_t _w[id][16]（16 字节，最大支持 64 位）
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
    int bit_width() const {
        return _bit_width;
    }

    // 连接引脚时调用，位宽取两者较大值
    void updateBitWidth(int pw) {
        _bit_width = std::max(_bit_width, pw);
    }

private:
    std::string _name;
    int _id;
    int _bit_width = 0;
};

} // namespace dsc
