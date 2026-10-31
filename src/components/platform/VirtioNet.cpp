#include "dcs/components/platform/VirtioNet.h"

#include "dcs/ComponentFactory.h"

#include <cstddef>
#include <cstring>
#include <format>

namespace dsc {

struct VirtioNet::State {
    BackendOps ops;
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
void resetThunk(void *ctx) {
    auto *s = static_cast<VirtioNet::State *>(ctx);
    if (s->ops.reset)
        s->ops.reset(s->ops.ctx);
}
} // namespace

VirtioNet::VirtioNet(const std::string &name) : SequentialComponent(name, "virtio_net"), _state(std::make_unique<State>()) {
    addInput("clk", 1);
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
    _state->ops = ops;
}

void VirtioNet::simInit() {
    if (_state->ops.init)
        _state->ops.init(_state->ops.ctx);
}

void VirtioNet::simDeinit() {
    if (_state->ops.deinit)
        _state->ops.deinit(_state->ops.ctx);
}

std::vector<Component::JitSymbol> VirtioNet::extraJitSymbols() const {
    return {{"dcs_virtio_reset", reinterpret_cast<void *>(&resetThunk)}};
}

bool VirtioNet::pushRxPacket(const uint8_t *data, uint32_t len) {
    auto &s = *_state;
    if (len > State::PKT_MAX)
        return false;
    if ((s.rx_head + 1) % State::PKT_RING == s.rx_tail % State::PKT_RING)
        return false;
    std::memcpy(s.rx_pkts[s.rx_head % State::PKT_RING], data, len);
    s.rx_len[s.rx_head % State::PKT_RING] = len;
    s.rx_head++;
    return true;
}

uint32_t VirtioNet::popTxPacket(uint8_t *buf, uint32_t max_len) {
    auto &s = *_state;
    if (s.tx_head == s.tx_tail)
        return 0;
    const uint32_t i = s.tx_tail % State::PKT_RING;
    uint32_t n = s.tx_len[i];
    if (n > max_len)
        n = max_len;
    std::memcpy(buf, s.tx_pkts[i], n);
    s.tx_tail++;
    return n;
}

void VirtioNet::pollBackend() {
    auto &s = *_state;
    uint8_t buf[State::PKT_MAX];
    if (s.ops.recv_pkt) {
        const uint32_t n = s.ops.recv_pkt(s.ops.ctx, buf, State::PKT_MAX);
        if (n)
            pushRxPacket(buf, n);
    }
    if (s.ops.send_pkt) {
        uint32_t n;
        while ((n = popTxPacket(buf, State::PKT_MAX)) != 0)
            s.ops.send_pkt(s.ops.ctx, buf, n);
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
    s.dev_id = 1;
    s.nq = 2;
    s.features = 0;
    for (int i = 0; i < 9; i++)
        s.in_nets.push_back((int) inputs()[i]->netId());
    s.state_decl = "";
    s.config_read = "";
    s.out_write = [this](int i, const std::string &e, uint32_t w) { return genOutputWrite(i, e, w); };

    QueueSpec rx;
    rx.wake = std::format(
            "        uint32_t _h = *(uint32_t *)((char *)_vio_state_{0} + {1});\n"
            "        uint32_t _t = *(uint32_t *)((char *)_vio_state_{0} + {2});\n"
            "        if (_h != _t) _qv->phase = {3};\n",
            s.id, rx_head_off, rx_tail_off, Q_AVAIL_IDX);
    // buffer 装不下 12 字节头时以 len=0 完成，包留在环里等下一个 buffer
    rx.desc = std::format("                _qv->phase = _qv->dlen < {} ? {} : {};\n", HDR, Q_USED, Q_DEV);
    rx.start = std::format(
            "        case {0}: {{ // 写全 0 的 virtio_net_hdr，包体按 buffer 剩余空间截断\n"
            "            uint32_t _t = *(uint32_t *)((char *)_vio_state_{1} + {2});\n"
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
            "            uint32_t _t = *(uint32_t *)((char *)_vio_state_{1} + {2});\n"
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
            "                    uint32_t _t = *(uint32_t *)((char *)_vio_state_{3} + {4});\n"
            "                    *(uint32_t *)((char *)_vio_state_{3} + {4}) = _t + 1;\n"
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
            "                uint32_t _th = *(uint32_t *)((char *)_vio_state_{1} + {2});\n"
            "                uint32_t _tl = *(uint32_t *)((char *)_vio_state_{1} + {3});\n"
            "                if (_th - _tl < {4} && _qv->written + _qv->chunk <= {5})\n"
            "                    dcs_memcpy((char *)_vio_state_{1} + {6} + (_th % {4}) * {5} + _qv->written,\n"
            "                               _vio_mp_buf_{1}, _qv->chunk);\n"
            "                _qv->written += _qv->chunk;\n"
            "                _qv->off += _qv->chunk;\n"
            "                if (_qv->off >= _qv->dlen) {{\n"
            "                    if (_qv->dflags & 1) {{ _qv->idx = _qv->dnext; _qv->off = 0; _qv->phase = {7}; }}\n"
            "                    else {{\n"
            // 整包收完：提交给宿主环；环满就丢弃，但 desc 一定要还回去
            "                        uint32_t _h2 = *(uint32_t *)((char *)_vio_state_{1} + {2});\n"
            "                        uint32_t _l2 = *(uint32_t *)((char *)_vio_state_{1} + {3});\n"
            "                        if (_h2 - _l2 < {4}) {{\n"
            "                            *(uint32_t *)((char *)_vio_state_{1} + {8} + (_h2 % {4}) * 4) = _qv->written;\n"
            "                            *(uint32_t *)((char *)_vio_state_{1} + {2}) = _h2 + 1;\n"
            "                        }}\n"
            "                        _qv->phase = {9};\n"
            "                    }}\n"
            "                }}\n"
            "                break;\n"
            "            }}\n",
            Q_DEV + 1, s.id, tx_head_off, tx_tail_off, PKT_RING, PKT_MAX, tx_pkts_off, Q_DESC, tx_len_off, Q_USED);

    s.queues.push_back(rx);
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
    return std::make_unique<VirtioNet>(new_name);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_net", {},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-net：队列 0 收包、队列 1 发包；宿主用 pushRxPacket/popTxPacket 或 BackendOps 对接"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::VirtioNet>(n);
    });
} // namespace
