#include "dcs/components/platform/VirtioNet.h"
#include "dcs/net/BackendOpts.h"

#include "dcs/ComponentFactory.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <unordered_map>

namespace dsc {

struct VirtioNet::State {
    // 后端上下文由 create 申请、destroy 释放，JIT 只负责在 guest 复位时转发
    void *backend_ctx = nullptr;
    void (*reset_fn)(void *) = nullptr;
    // JIT 往 TX 环推入包后叫醒后端线程
    void (*kick_fn)(void *) = nullptr;
    // 宿主与设备之间的包环，JIT 直接读写
    static constexpr uint32_t PKT_RING = 8;
    static constexpr uint32_t PKT_MAX = 1600; // 1514 以太网帧 + 余量
    uint8_t rx_pkts[PKT_RING][PKT_MAX] = {};
    uint32_t rx_len[PKT_RING] = {};
    uint32_t rx_head = 0; // 宿主写
    uint32_t rx_tail = 0; // 设备读
    uint8_t tx_pkts[PKT_RING][PKT_MAX] = {};
    uint32_t tx_len[PKT_RING] = {};
    uint32_t tx_head = 0; // 设备写
    uint32_t tx_tail = 0; // 宿主读
};

namespace {
// 生成代码里调用的复位回调名。JIT 符号表按名字索引，必须和 register 时用的字符串一致
constexpr const char *kResetSymbol = "dcs_virtio_net_reset";

std::unordered_map<std::string, VirtioNet::BackendOps> &backendRegistry() {
    static std::unordered_map<std::string, VirtioNet::BackendOps> m;
    return m;
}

void resetThunk(void *ctx) {
    auto *s = static_cast<VirtioNet::State *>(ctx);
    // backend_ctx 为空表示后端创建失败：不能假设后端实现自己会做空检查
    if (s->reset_fn && s->backend_ctx)
        s->reset_fn(s->backend_ctx);
}
} // namespace

void VirtioNet::kickThunk(void *state) {
    auto *s = static_cast<State *>(state);
    // backend_ctx 为空表示后端创建失败；不能假设后端实现自己会做空检查
    if (s->kick_fn && s->backend_ctx)
        s->kick_fn(s->backend_ctx);
}

void VirtioNet::registerBackend(const std::string &name, const BackendOps &ops) {
    backendRegistry()[name] = ops;
}

VirtioNet::VirtioNet(const std::string &name, const std::string &backend,
                     std::map<std::string, std::string> backend_opts) :
    SequentialComponent(name, "virtio_net"), _state(std::make_unique<State>()), _backend(backend),
    _backend_opts(std::move(backend_opts)) {
    setParam("backend", backend);
    // 后端私有选项按 backend_opts.<key> 记进参数表：JSON 导出/回读仍然是扁平键，
    // 嵌套写法只是加载时的一种便利（见 dcs/net/BackendOpts.h）
    for (auto &[k, v]: _backend_opts)
        setParam(std::string(kBackendOptsPrefix) + k, v);
    addInput("clk", 1, PinEdge::Rising);
    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("m_dat_i", 32);
    addInput("m_ack_i", 1);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("irq", 1, false, true);
    addOutput("m_adr_o", 32, false, true);
    addOutput("m_dat_o", 32, false, true);
    addOutput("m_we_o", 1, false, true);
    addOutput("m_sel_o", 4, false, true);
    addOutput("m_stb_o", 1, false, true);
    addOutput("m_cyc_o", 1, false, true);
}

VirtioNet::~VirtioNet() = default;

void VirtioNet::setBackend(const BackendOps &ops) {
    _ops = ops;
}

bool VirtioNet::prepareImpl(Context &, std::string &error) {
    if (_backend == "none" || _ops.create || _backend_missing)
        return true;
    auto &reg = backendRegistry();
    auto it = reg.find(_backend);
    if (it == reg.end()) {
        // 精简构建可能没把网络后端编进来（DCS_BUILD_NET=OFF）。整机启动、
        // 块设备、控制台都不依赖网卡，缺后端时退化成 none 并告警，而不是
        // 让电路直接加载失败
        std::fprintf(stderr, "[dcs] 警告: virtio_net 后端 '%s' 未注册，按 none 运行（guest 无网络）\n",
                     _backend.c_str());
        // 只记降级状态，不改 _backend：exportParams/clone()/backendName()
        // 都要保留用户写的原始配置，换个构建加载仍能恢复
        _backend_missing = true;
        return true;
    }
    _ops = it->second;
    return true;
}

void VirtioNet::simInit() {
    // init 被当作"重置"重复调用时，旧后端已经起了线程并在消费同一个
    // TX/RX 环；必须先销毁它再 create，否则旧线程失去引用、永不停止
    if (_ops.destroy && _state->backend_ctx) {
        _ops.destroy(_state->backend_ctx);
        _state->backend_ctx = nullptr;
    }
    // 重新 init 等于冷启动：上轮残留的 RX/TX 包不能投递给重启后的 guest。
    // 旧后端线程已经停掉，这里没有并发访问
    _state->rx_head = _state->rx_tail = 0;
    _state->tx_head = _state->tx_tail = 0;
    // create 拿到设备本身，后端可以在自己的线程里直接 pushRxPacket/popTxPacket
    _state->backend_ctx = _ops.create ? _ops.create(this) : nullptr;
    _state->reset_fn = _ops.reset;
    // create 失败（socket 或 slirp 初始化失败）时 backend_ctx 为空，不能留 kick：
    // 驱动照常写 notify，kickThunk 会拿空指针调进后端实现
    _state->kick_fn = _state->backend_ctx ? _ops.kick : nullptr;
}

void VirtioNet::simDeinit() {
    if (_ops.destroy && _state->backend_ctx) {
        _ops.destroy(_state->backend_ctx);
        _state->backend_ctx = nullptr;
    }
}

std::vector<Component::JitSymbol> VirtioNet::extraJitSymbols() const {
    return {
            {kResetSymbol, reinterpret_cast<void *>(&resetThunk)},
            {"dcs_vnet_kick", reinterpret_cast<void *>(&VirtioNet::kickThunk)},
            {"dcs_virtio_trace", reinterpret_cast<void *>(&dcs_virtio_trace)},
    };
}

bool VirtioNet::pushRxPacket(const uint8_t *data, uint32_t len) {
    // pollBackend 和后端线程都可能投递 RX 包；环本身只保证单生产者
    std::lock_guard<std::mutex> lock(_rx_mutex);
    auto &s = *_state;
    // 0 长度帧没有意义，而且设备侧会把它当"头写完但没有包体"：rx_tail 不前进，
    // 后面所有包都被它堵在环里
    if (len == 0 || len > State::PKT_MAX)
        return false;
    // 单生产者环：后端线程写 head，JIT 写 tail，两边用 acquire/release 交接数据
    const uint32_t head = std::atomic_ref<uint32_t>(s.rx_head).load(std::memory_order_relaxed);
    const uint32_t tail = std::atomic_ref<uint32_t>(s.rx_tail).load(std::memory_order_acquire);
    // 差值判满，避免 head+1 在 UINT32_MAX 处回绕导致的误判，见 VirtioInput::pushInputEvent
    const uint32_t used = head - tail;
    if (used >= State::PKT_RING - 1)
        return false;
    std::memcpy(s.rx_pkts[head % State::PKT_RING], data, len);
    s.rx_len[head % State::PKT_RING] = len;
    std::atomic_ref<uint32_t>(s.rx_head).store(head + 1, std::memory_order_release);
    return true;
}

uint32_t VirtioNet::popTxPacket(uint8_t *buf, uint32_t max_len) {
    // pollBackend 和后端线程都可能取 TX 包；环本身只保证单消费者
    std::lock_guard<std::mutex> lock(_tx_mutex);
    auto &s = *_state;
    const uint32_t head = std::atomic_ref<uint32_t>(s.tx_head).load(std::memory_order_acquire);
    const uint32_t tail = std::atomic_ref<uint32_t>(s.tx_tail).load(std::memory_order_relaxed);
    if (head == tail)
        return 0;
    const uint32_t i = tail % State::PKT_RING;
    uint32_t n = s.tx_len[i];
    if (n > max_len)
        n = max_len;
    std::memcpy(buf, s.tx_pkts[i], n);
    std::atomic_ref<uint32_t>(s.tx_tail).store(tail + 1, std::memory_order_release);
    return n;
}

void VirtioNet::pollBackend() {
    void *ctx = _state->backend_ctx;
    if (!ctx)
        return;
    uint8_t buf[State::PKT_MAX];
    if (_ops.recv_pkt) {
        const uint32_t n = _ops.recv_pkt(ctx, buf, State::PKT_MAX);
        if (n)
            pushRxPacket(buf, n);
    }
    if (_ops.send_pkt) {
        uint32_t n;
        while ((n = popTxPacket(buf, State::PKT_MAX)) != 0)
            _ops.send_pkt(ctx, buf, n);
    }
}

virtio_gen::DeviceSpec VirtioNet::makeSpec() const {
    using namespace virtio_gen;
    constexpr uint32_t HDR = 12; // virtio_net_hdr
    constexpr uint32_t PKT_RING = State::PKT_RING;
    constexpr uint32_t PKT_MAX = State::PKT_MAX;

    const unsigned long long rx_head_off = offsetof(State, rx_head);
    const unsigned long long rx_tail_off = offsetof(State, rx_tail);
    const unsigned long long rx_len_off = offsetof(State, rx_len);
    const unsigned long long rx_pkts_off = offsetof(State, rx_pkts);
    const unsigned long long tx_head_off = offsetof(State, tx_head);
    const unsigned long long tx_tail_off = offsetof(State, tx_tail);
    const unsigned long long tx_len_off = offsetof(State, tx_len);
    const unsigned long long tx_pkts_off = offsetof(State, tx_pkts);

    DeviceSpec s;
    s.id = std::to_string(_id);
    s.func_name = funcName_seq();
    s.node_name = std::format("VirtioNet {}", name());
    s.reset_symbol = kResetSymbol;
    s.dev_id = 1;
    s.nq = 2;
    s.features = 0;
    for (int i = 0; i < virtio_gen::IN_COUNT; i++)
        s.in_nets[i] = (int) inputs()[i]->netId();
    s.state_decl = "extern void dcs_vnet_kick(void *);\n";
    s.config_read = "";
    s.out_write = [this](int i, const std::string &e, uint32_t w) { return genOutputWrite(i, e, w); };

    QueueSpec rx;
    rx.wake = std::format(
            "        uint32_t _h = dcs_load_acq((char *)_vio_state_{0} + {1});\n"
            "        uint32_t _t = dcs_load_acq((char *)_vio_state_{0} + {2});\n"
            "        if (_h != _t) _qv->phase = {3};\n",
            s.id, rx_head_off, rx_tail_off, Q_AVAIL_IDX);
    // 只有真的有包时才消费描述符：驱动的 avail 环里常驻几十个空 buffer，
    // 若逐个消费，rx_tail 会越过 rx_head，之后 `h != t` 永远成立，
    // 设备就会无限投递空包（used.idx 一路涨）。没有包时回到空闲，等 wake 再启动
    //
    // buffer 装不下 12 字节头时只归还描述符、不推进 rx_tail：包留在环里等驱动下次
    // 提供足够大的 buffer（测试 TinyRxBufferKeepsPacket 锁着这个语义）。这里刻意
    // 不丢包，因为丢弃对驱动不可见，等于静默吞帧
    rx.desc = std::format(
            "                uint32_t _rh = dcs_load_acq((char *)_vio_state_{0} + {1});\n"
            "                uint32_t _rt = dcs_load_acq((char *)_vio_state_{0} + {2});\n"
            "                if (_rh == _rt) _qv->phase = 0;\n"
            "                else _qv->phase = _qv->dlen < {3} ? {4} : {5};\n",
            s.id, rx_head_off, rx_tail_off, HDR, Q_USED, Q_DEV);
    rx.start = std::format(
            "        case {0}: {{ // 写全 0 的 virtio_net_hdr，包体按 buffer 剩余空间截断\n"
            "            uint32_t _t = dcs_load_acq((char *)_vio_state_{1} + {2});\n"
            "            uint32_t _n = *(uint32_t *)((char *)_vio_state_{1} + {3} + (_t % {4}) * 4);\n"
            "            uint32_t _cap = _qv->dlen - {5};\n"
            "            if (_n > _cap) _n = _cap;\n"
            "            if (_n > {6}) _n = {6};\n"
            "            _qv->pkt = _n;\n"
            "            _qv->written = {5} + _n;\n"
            "            dcs_memset(_vio_mp_buf_{1}, 0, {5});\n"
            "            _a = _qv->daddr; _l = {5}; _wr = 1;\n"
            "            break;\n"
            "        }}\n"
            "        case {7}: {{\n"
            "            uint32_t _t = dcs_load_acq((char *)_vio_state_{1} + {2});\n"
            "            uint32_t _c = _qv->pkt - _qv->off;\n"
            "            if (_c > 64) _c = 64;\n"
            "            _qv->chunk = _c;\n"
            "            dcs_memcpy(_vio_mp_buf_{1}, (char *)_vio_state_{1} + {8} + (_t % {4}) * {6} + _qv->off, _c);\n"
            "            _a = _qv->daddr + {5} + _qv->off; _l = _c; _wr = 1;\n"
            "            break;\n"
            "        }}\n",
            Q_DEV, s.id, rx_tail_off, rx_len_off, PKT_RING, HDR, PKT_MAX, Q_DEV + 1, rx_pkts_off);
    rx.done = std::format(
            "            case {0}: {{ // 头部写完\n"
            "                _qv->phase = _qv->pkt == 0 ? {1} : {2};\n"
            "                break;\n"
            "            }}\n"
            "            case {2}: {{ // 包体一块写完\n"
            "                _qv->off += _qv->chunk;\n"
            "                if (_qv->off >= _qv->pkt) {{\n"
            "                    uint32_t _t = dcs_load_acq((char *)_vio_state_{3} + {4});\n"
            "                    dcs_store_rel((char *)_vio_state_{3} + {4}, _t + 1);\n"
            "                    _qv->phase = {1};\n"
            "                }}\n"
            "                break;\n"
            "            }}\n",
            Q_DEV, Q_USED, Q_DEV + 1, s.id, rx_tail_off);

    QueueSpec tx;
    tx.wake = "";
    // 链上第一个 desc 的前 12 字节是 virtio_net_hdr，跳过；后续 desc 全是包体
    tx.desc = std::format(
            "                if (_qv->step == 0) {{\n"
            "                    _qv->step = 1;\n"
            "                    _qv->off = _qv->dlen < {0} ? _qv->dlen : {0};\n"
            "                }} else _qv->off = 0;\n"
            "                if (_qv->dlen <= _qv->off) {{\n"
            "                    if (_qv->dflags & 1) {{ _qv->idx = _qv->dnext; _qv->phase = {1}; }}\n"
            "                    else _qv->phase = {2};\n"
            "                }} else _qv->phase = {3};\n",
            HDR, Q_DESC, Q_USED, Q_DEV + 1);
    tx.start = std::format(
            "        case {0}: {{\n"
            "            uint32_t _c = _qv->dlen - _qv->off;\n"
            "            if (_c > 64) _c = 64;\n"
            "            _qv->chunk = _c;\n"
            "            _a = _qv->daddr + _qv->off; _l = _c; _wr = 0;\n"
            "            break;\n"
            "        }}\n",
            Q_DEV + 1);
    tx.done = std::format(
            "            case {0}: {{ // 包体一块读回，追加进宿主环\n"
            "                uint32_t _th = dcs_load_acq((char *)_vio_state_{1} + {2});\n"
            "                uint32_t _tl = dcs_load_acq((char *)_vio_state_{1} + {3});\n"
            // 超过 PKT_MAX 的包只拷能放下的部分，长度也必须按实际拷贝量截断；
            // 只跳过 memcpy 却继续累加 written 会让 tx_len 指向没写过的残留数据
            "                uint32_t _c = _qv->chunk;\n"
            "                if (_qv->written >= {5}) _c = 0;\n"
            "                else if (_qv->written + _c > {5}) _c = {5} - _qv->written;\n"
            "                if (_th - _tl < {4} && _c)\n"
            "                    dcs_memcpy((char *)_vio_state_{1} + {6} + (_th % {4}) * {5} + _qv->written,\n"
            "                               _vio_mp_buf_{1}, _c);\n"
            "                _qv->written += _c;\n"
            "                _qv->off += _qv->chunk;\n"
            "                if (_qv->off >= _qv->dlen) {{\n"
            "                    if (_qv->dflags & 1) {{ _qv->idx = _qv->dnext; _qv->off = 0; _qv->phase = {7}; }}\n"
            "                    else {{\n"
            // 整包收完：提交给宿主环；环满就丢弃，但 desc 一定要还回去
            "                        uint32_t _h2 = dcs_load_acq((char *)_vio_state_{1} + {2});\n"
            "                        uint32_t _l2 = dcs_load_acq((char *)_vio_state_{1} + {3});\n"
            "                        uint32_t _n = _qv->written > {5} ? {5} : _qv->written;\n"
            "                        if (_h2 - _l2 < {4}) {{\n"
            "                            *(uint32_t *)((char *)_vio_state_{1} + {8} + (_h2 % {4}) * 4) = _n;\n"
            "                            dcs_store_rel((char *)_vio_state_{1} + {2}, _h2 + 1);\n"
            // 只在真的推入一个包时叫醒后端，和每拍无关
            "                            dcs_virtio_trace({1}u, 0x306u, _h2, _l2, _n, 0, 0, 0);\n"
            "                            dcs_vnet_kick(_vio_state_{1});\n"
            "                        }} else _n = 0;\n"
            // used.len 由公共 genQueue 从 _qv->written 写回：环满时整包丢弃，
            // 必须报 0 字节交付，否则驱动会认为这个缓冲区已经发送成功并回收
            "                        _qv->written = _n;\n"
            "                        _qv->phase = {9};\n"
            "                    }}\n"
            "                }}\n"
            "                break;\n"
            "            }}\n",
            Q_DEV + 1, s.id, tx_head_off, tx_tail_off, PKT_RING, PKT_MAX, tx_pkts_off, Q_DESC, tx_len_off, Q_USED);

    s.queues.push_back(rx);
    // RX 只是补 buffer，真正的"有包"由 wake 按 rx_head/rx_tail 判断
    s.queues.back().notify_kick = false;
    s.queues.push_back(tx);
    return s;
}

std::string VirtioNet::genStateDecl() const {
    return virtio_gen::genStateDecl(makeSpec());
}

std::string VirtioNet::genFuncDef_seq() const {
    return virtio_gen::genSeq(makeSpec());
}

std::string VirtioNet::genInitCode() const {
    return virtio_gen::genInitCode(makeSpec(), _state.get());
}

std::unique_ptr<Component> VirtioNet::clone(const std::string &new_name) const {
    // 外部注入的 BackendOps 不随 clone 复制：clone 用于复合元件展开，
    // 宿主注入的上下文属于原实例，复制会让两个实例重复销毁同一个后端
    return std::make_unique<VirtioNet>(new_name, _backend, _backend_opts);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_net", {{"backend", "none", {"none", "slirp"}}, {"backend_opts", ""}},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-net：队列 0 收包、队列 1 发包；宿主用 pushRxPacket/popTxPacket 或 BackendOps 对接", true},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::VirtioNet>(n, dsc::paramStr(p, "backend", "none"), dsc::takeBackendOpts(p));
    });
} // namespace
