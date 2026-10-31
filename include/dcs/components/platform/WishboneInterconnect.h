// WishboneInterconnect — Wishbone 地址解码器 / MMIO 路由器
//
// 将一个 Wishbone 主设备连接到多个 Wishbone 从设备，按地址段路由。
// 路由逻辑在 C++ 回调中实现，JIT C 代码仅负责引脚搬运。
//
// 地址映射: 每个从设备一个 [base, base+size) 地址段，按注册顺序匹配。
//
// 主设备侧引脚 (面对 CPU):
//   输入: m_adr_i(32), m_dat_i(32), m_we_i(1), m_sel_i(4), m_stb_i(1), m_cyc_i(1)
//         + 每个从设备的 sX_dat_i(32), sX_ack_i(1)
//   输出: m_dat_o(32), m_ack_o(1)
//         + 每个从设备的 sX_adr_o(32), sX_dat_o(32), sX_we_o(1), sX_sel_o(4),
//           sX_stb_o(1), sX_cyc_o(1)
//
// 参数:
//   segments: "0:65536,65536:4096" 格式的地址段描述
//             每个段 "base:size"，多个段用逗号分隔
//             第一个段为默认段（未匹配时路由到此）
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class WishboneInterconnect : public Component {
public:
    // segments: 地址段描述字符串，格式 "base:size,base:size,..."
    // num_masters: 主设备数量，主侧引脚依次为 m0_*, m1_*, ...，多个主设备按轮询仲裁。
    WishboneInterconnect(const std::string &name, const std::string &segments = "0:65536", int num_masters = 1);
    ~WishboneInterconnect() override;

    struct Segment {
        uint32_t base;
        uint32_t size; // 字节数
    };
    const std::vector<Segment> &segments() const { return _segments; }
    int numSlaves() const { return (int) _segments.size(); }
    int numMasters() const { return _masters; }

    // ---- Component 接口 ----
    bool hasCombinational() const override { return false; }
    bool hasSequential() const override { return true; }

    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genResetCode() const override { return ""; }
    std::string genDeinitCode() const override { return ""; }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    std::vector<Segment> _segments;
    int _masters = 1;
    bool _prepared = false;
};

} // namespace dsc
