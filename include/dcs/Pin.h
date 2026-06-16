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
    Pin(const std::string &name, PinDir dir, int bit_width, Component *parent);

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

    // 连接的线网（nullptr = 悬空，悬空输入视为 0）
    Net *net() const {
        return _net;
    }
    void setNet(Net *n) {
        _net = n;
    }

    // 便捷方法：返回连接线网的 id（-1 = 悬空）
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
    int _bit_width; // 1~64
    Component *_parent;
    Net *_net = nullptr;
};

} // namespace dsc
