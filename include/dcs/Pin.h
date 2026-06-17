#pragma once
#include <cstdint>
#include <string>
#include "Net.h"

namespace dsc {

class Component; // 仅需前向声明，Pin 不调用 Component 的成员

// 引脚方向
enum class PinDir { INPUT, OUTPUT };

// ============================================================
// Pin — 元件的连接端点
//
// 每个引脚有固定方向（输入/输出）和位宽（1~64）。
// 连接到 Net 后，Net 的位宽会被自动更新为所有连接引脚的最大位宽。
//
// 输入引脚: 读取线网低 N 位作为输入值（N = 引脚位宽）
// 输出引脚: 将元件计算结果写入线网低 N 位
// 悬空引脚: net() 返回 nullptr，netId() 返回 -1
// ============================================================
class Pin {
public:
    // parent: 所属元件指针
    // is_tri_state: 输出引脚是否为三态（仅输出有效）
    // is_sequential: 输出引脚是否依赖内部状态、打破组合环路（仅输出有效）
    Pin(const std::string &name, PinDir dir, int bit_width, Component *parent, bool is_tri_state = false,
        bool is_sequential = false);

    const std::string &name() const {
        return _name;
    }
    PinDir dir() const {
        return _dir;
    }
    int bit_width() const {
        return _bit_width;
    }
    Component *parent() const {
        return _parent;
    }

    bool isTriState() const {
        return _is_tri_state;
    }
    bool isSequential() const {
        return _is_sequential;
    }
    void setTriState(bool v) {
        _is_tri_state = v;
    }
    void setSequential(bool v) {
        _is_sequential = v;
    }

    Net *net() const {
        return _net;
    }
    void setNet(Net *n) {
        _net = n;
    }
    int netId() const {
        return _net ? _net->id() : -1;
    }
    bool isInput() const {
        return _dir == PinDir::INPUT;
    }
    bool isOutput() const {
        return _dir == PinDir::OUTPUT;
    }

private:
    std::string _name;
    PinDir _dir;
    int _bit_width;
    Component *_parent;
    Net *_net = nullptr;
    bool _is_tri_state = false;
    bool _is_sequential = false;
};

} // namespace dsc
