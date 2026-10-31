// WishboneROMAdapter — ROM 的 Wishbone 从设备适配器
//
// 作为独立元件插入 ROM 与 Wishbone 总线之间，通过网表连接：
//   适配器 rom_addr 输出 → ROM addr 输入
//   ROM data_out 输出   → 适配器 rom_data 输入
//
// 适配器本身不存储数据、不加载文件，仅做协议转换：
//   Wishbone stb/ack 握手 ↔ ROM 的 addr/data 时序
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class WishboneROMAdapter : public SequentialComponent {
public:
    // addr_width:  地址位宽（须与 ROM 的 addr_width 一致）
    // data_width:  数据位宽（须与 ROM 的 data_width 一致）
    // read_latency: 读延迟拍数（须与 ROM 的 read_latency 一致）
    WishboneROMAdapter(const std::string &name, int addr_width, int data_width,
                       int read_latency = 0);

    // ---- Component 接口 ----
    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _addr_width;
    int _data_width;
    int _read_latency;
    int _d_bytes; // data_width / 8
};

} // namespace dsc
