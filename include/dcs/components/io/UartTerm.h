// UartTerm — 串口终端接收端
//
// 只做解串，不负责输出：把一条 8N1 串行线上的电平解成字节，从 data 输出，
// valid 拉高一拍表示这拍有有效字节。真正怎么显示交给下游（通常是 print 元件），
// 这样串口解码和打印格式两件事各自独立。
//
// 解码时序和 UART 的接收端完全一致（起始沿 -> 半位确认 -> 每位中点采样 ->
// 停止位校验），因此 baud_div 必须和对面 UART 的构造参数相同。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class UartTerm : public SequentialComponent {
public:
    explicit UartTerm(const std::string &name, int baud_div = 8);
    ~UartTerm() override;

    std::vector<JitSymbol> extraJitSymbols() const override { return {}; }

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 收到完整帧的计数，帧错误（停止位为低）的计数，测试里用。
    // 这几个值由 JIT 代码直接写内存，外部查询接口保持不变
    uint64_t frames() const { return _frames; }
    uint64_t errors() const { return _errors; }
    // 最后一帧解出的字节。测试靠它验证解码结果，不必去捕获 stdout
    uint8_t lastByte() const { return _last_byte; }

private:
    uint64_t _frames = 0;
    uint64_t _errors = 0;
    uint8_t _last_byte = 0;
    int _baud_div = 8;
};

} // namespace dsc
