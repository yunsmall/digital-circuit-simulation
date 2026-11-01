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

// 设备卡死检测的宿主回调；实现在 VirtioCommon.cpp，各 virtio 元件把它注册进 JIT
extern "C" void dcs_virtio_trace(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

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

// QueueNumMax。Linux 的 virtio-net 在 num_free < 2 + MAX_SKB_FRAGS（约 19）时
// 会直接停掉 TX 队列，环太小会导致"发包即卡死、再无 notify"，所以必须给足
inline constexpr uint32_t QUEUE_MAX = 256;

struct QueueSpec {
    // phase==0 且有活干时把 _qv->phase 设成 Q_AVAIL_IDX；可用 _qv
    std::string wake;
    // 描述符字段读完后决定下一步阶段（必须设置 _qv->phase）
    std::string desc;
    // 设备特有阶段的启动代码：设置 _a / _l / _wr
    std::string start;
    // 设备特有阶段的完成代码：推进 _qv->phase
    std::string done;
    // notify 写这个队列时是否直接启动。RX/事件这类"驱动只是补 buffer"的队列
    // 必须设 false，否则每次 refill 的 notify 都会被当成本地请求去消费空描述符
    bool notify_kick = true;
};

struct DeviceSpec {
    std::string id;        // 元件实例 id，JIT 变量名后缀
    std::string func_name; // genFuncDef_seq 的函数名
    std::string node_name; // 注释用
    // 生成代码调用的复位回调符号名。JIT 符号表按名字建索引，同名只会留下最后
    // 注册的那个；各设备的 State 布局不同、thunk 也不同，所以每个设备类必须用自己的名字
    std::string reset_symbol = "dcs_virtio_reset";
    uint32_t dev_id = 0;
    uint32_t nq = 1;
    uint32_t features = 0;
    std::vector<int> in_nets; // 9 个输入引脚的线网 id
    std::string state_decl;   // 设备私有 static 变量（含换行）
    std::string init_code;    // 设备私有初始化（含缩进和换行）
    std::string config_read;  // 0x100 起的读分支，形如 "case 0x100: ...; break;\n"
    // 0x100 起的写分支（可选）；代码里可用 _sel 判断写入的字节通道
    std::string config_write;
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
    // 复位回调符号名：各设备类必须不同，原因见 DeviceSpec::reset_symbol
    std::string reset_symbol = "dcs_virtio_reset";
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
