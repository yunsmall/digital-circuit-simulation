// WbMasterAdapter — 总线中立的存储器事务接口 → Wishbone 主设备
//
// CPU 等主设备只产生 mem_* 事务（mem_req 对应 STB，mem_lock 表示事务仍占用
// 总线，对应 CYC），本适配器把它映射到 Wishbone。请求在时序阶段紧跟主设备
// 执行（组件顺序必须放在主设备之后），应答在组合阶段透传，主设备在时序阶段
// 读到的 ack/rdat 时序与原来的直连 Wishbone 完全一致。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class WbMasterAdapter : public Component {
public:
    explicit WbMasterAdapter(const std::string &name);
    ~WbMasterAdapter() override;

    // 请求方向是时序的（必须等主设备本拍写完 _w），应答方向是组合的
    bool hasCombinational() const override {
        return true;
    }
    bool hasSequential() const override {
        return true;
    }

    std::string genFuncDef_comb() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;
};

} // namespace dsc
