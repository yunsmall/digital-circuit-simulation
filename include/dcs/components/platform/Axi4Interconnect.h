// Axi4Interconnect — 多主多从 AXI4 互连
//
// 按地址段把主口事务路由到从口，并按 A5.2.3 在 AWID/ARID 高位追加主口编号，
// 响应时用 BID/RID 高位选回主口并剥离附加位。互连内部同一时刻只处理一笔写事务
// 和一笔读事务（写通道与读通道相互独立），对每个主口保持事务顺序。
//
// 未命中任何地址段的访问由互连自身返回 DECERR：写事务收完所有 W beat 后回
// BRESP=DECERR，读事务按 ARLEN+1 个 beat 返回 RRESP=DECERR（A3.4.5 要求即使
// 出错也必须完成全部传输）。
//
// AXI 输入输出之间不允许组合路径（A3.1.1）：所有转发字段先锁存进内部寄存器，
// 再驱动下游通道。
#pragma once
#include <string>
#include <vector>

#include "dcs/Component.h"

namespace dsc {

class Axi4Interconnect : public SequentialComponent {
public:
    // segments 形如 "0:4096,4096:256"，每个段对应一个从口
    Axi4Interconnect(const std::string &name, const std::string &segments, int num_masters, int id_width = 4);
    ~Axi4Interconnect() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    struct Segment {
        uint64_t base;
        uint64_t size;
    };
    std::vector<Segment> _segments;
    int _num_masters;
    int _id_width;
    int _extra_bits;
    int _slave_id_width;

    int masterInIdx(int m, int which) const;
    int masterOutIdx(int m, int which) const;
    int slaveInIdx(int s, int which) const;
    int slaveOutIdx(int s, int which) const;
    std::string genDecode(const std::string &addr_var, int out_var) const;
};

} // namespace dsc
