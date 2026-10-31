// virtio 传输层的生成期拼装。
//
// 这里没有类，也没有运行时多态：寄存器段、主端口 Wishbone 事务、virtqueue
// 公共骨架（avail.idx → avail.ring → desc → used）全部由自由函数拼成 C 代码，
// 设备元件只提供四段生成期片段（唤醒 / desc 决策 / 设备阶段启动 / 阶段推进）。
// 产出的代码在 JIT 里就是普通函数，没有中间层。
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dsc::virtio_gen {

// 从端口 / 主端口引脚顺序，和设备元件的 addInput/addOutput 顺序一致
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_M_DAT, I_M_ACK };
enum OutIdx { O_DAT = 0, O_ACK, O_IRQ, O_M_ADR, O_M_DAT, O_M_WE, O_M_SEL, O_M_STB, O_M_CYC };

// virtqueue 公共阶段；设备特有阶段从 Q_DEV 起自己分配。
// 用 int 常量而不是 enum：这些值要直接当 std::format 的参数拼进生成代码
inline constexpr int Q_IDLE = 0;
inline constexpr int Q_AVAIL_IDX = 1;
inline constexpr int Q_AVAIL_RING = 2;
inline constexpr int Q_DESC = 3;
inline constexpr int Q_USED = 7;
inline constexpr int Q_USED_IDX = 8;
inline constexpr int Q_DEV = 10;

inline constexpr uint32_t QUEUE_MAX = 8;

struct QueueSpec {
    // phase==0 且有活干时把 _qv->phase 设成 Q_AVAIL_IDX；可用 _qv
    std::string wake;
    // 描述符字段读完后决定下一步阶段（必须设置 _qv->phase）
    std::string desc;
    // 设备特有阶段的启动代码：设置 _a / _l / _wr
    std::string start;
    // 设备特有阶段的完成代码：推进 _qv->phase
    std::string done;
};

struct DeviceSpec {
    std::string id;        // 元件实例 id，JIT 变量名后缀
    std::string func_name; // genFuncDef_seq 的函数名
    std::string node_name; // 注释用
    uint32_t dev_id = 0;
    uint32_t nq = 1;
    uint32_t features = 0;
    std::vector<int> in_nets; // 9 个输入引脚的线网 id
    std::string state_decl;   // 设备私有 static 变量（含换行）
    std::string init_code;    // 设备私有初始化（含缩进和换行）
    std::string config_read;  // 0x100 起的读分支，形如 "case 0x100: ...; break;\n"
    std::vector<QueueSpec> queues;
    // 输出写语句生成器（封装 Component::genOutputWrite）
    std::function<std::string(int out_idx, const std::string &expr, uint32_t width)> out_write;
};

std::string genStateDecl(const DeviceSpec &spec);
std::string genInitCode(const DeviceSpec &spec, void *state);
std::string genSeq(const DeviceSpec &spec);

// ---- virtio-blk 的请求解析（两个 blk 元件共用） ----
// 介质访问语句由调用方提供，代码里可用 _io_off（介质字节偏移）和 _c（本块长度），
// read_stmt 负责把 _c 字节读进 _vio_mp_buf_<id>，write_stmt 反之；
// 失败时由语句自己设置 _vio_status_byte_<id>。
struct BlkParams {
    std::string id;
    std::string func_name;
    std::string node_name;
    std::vector<int> in_nets;
    uint32_t features = 0;
    uint64_t size_bytes = 0;
    std::string state_decl; // 设备私有变量，必须含 _vio_type_<id>/_vio_sec_<id>/_vio_status_byte_<id>
    std::string init_code;
    std::string read_stmt;
    std::string write_stmt;
    std::function<std::string(int out_idx, const std::string &expr, uint32_t width)> out_write;
};

DeviceSpec makeBlkSpec(const BlkParams &p);

// 队列字段访问表达式：_vio_q_<id>[q].<field>
std::string qv(const std::string &id, int q, const char *field);

} // namespace dsc::virtio_gen
