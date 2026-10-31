#include "dcs/components/platform/VirtioInput.h"

#include "dcs/ComponentFactory.h"

#include <cstddef>
#include <format>

namespace dsc {

struct VirtioInput::State {
    BackendOps ops;
    // 宿主 push 的事件环：dcs 是单线程的，宿主只在两次 tick 之间 push，
    // JIT 直接读 head/tail，不用跨 JIT 回调
    static constexpr uint32_t EVT_RING = 64;
    InputEvent events[EVT_RING] = {};
    uint32_t evt_head = 0; // 宿主写
    uint32_t evt_tail = 0; // 设备读
};

namespace {
void resetThunk(void *ctx) {
    auto *s = static_cast<VirtioInput::State *>(ctx);
    if (s->ops.reset)
        s->ops.reset(s->ops.ctx);
}
} // namespace

VirtioInput::VirtioInput(const std::string &name) :
    SequentialComponent(name, "virtio_input"), _state(std::make_unique<State>()) {
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

VirtioInput::~VirtioInput() = default;

void VirtioInput::setBackend(const BackendOps &ops) {
    _state->ops = ops;
}

void VirtioInput::simInit() {
    if (_state->ops.init)
        _state->ops.init(_state->ops.ctx);
}

void VirtioInput::simDeinit() {
    if (_state->ops.deinit)
        _state->ops.deinit(_state->ops.ctx);
}

std::vector<Component::JitSymbol> VirtioInput::extraJitSymbols() const {
    return {{"dcs_virtio_reset", reinterpret_cast<void *>(&resetThunk)}};
}

bool VirtioInput::pushInputEvent(const InputEvent &ev) {
    auto &s = *_state;
    const uint32_t next = (s.evt_head + 1) % State::EVT_RING;
    if (next == s.evt_tail)
        return false; // 环满
    s.events[s.evt_head] = ev;
    s.evt_head = next;
    return true;
}

virtio_gen::DeviceSpec VirtioInput::makeSpec() const {
    using namespace virtio_gen;
    DeviceSpec s;
    s.id = std::to_string(_id);
    s.func_name = funcName_seq();
    s.node_name = std::format("VirtioInput {}", name());
    s.dev_id = 18;
    s.nq = 1;
    s.features = 0;
    for (int i = 0; i < 9; i++)
        s.in_nets.push_back((int) inputs()[i]->netId());
    s.state_decl = "";
    s.config_read = "";
    s.out_write = [this](int i, const std::string &e, uint32_t w) { return genOutputWrite(i, e, w); };

    const unsigned long long head_off = offsetof(State, evt_head);
    const unsigned long long tail_off = offsetof(State, evt_tail);
    const unsigned long long evt_off = offsetof(State, events);
    const unsigned ring = State::EVT_RING;

    // 生成期展开 8 个事件槽：常量下标，没有循环
    std::string copy_events;
    for (int k = 0; k < 8; k++)
        copy_events += std::format(
                "            if ({0}u < _n) dcs_memcpy(_vio_mp_buf_{2} + {1}, "
                "(char *)_vio_state_{2} + {3} + ((_t + {0}u) % {4}u) * 8, 8);\n",
                k, k * 8, s.id, evt_off, ring);

    QueueSpec q;
    q.wake = std::format(
            "        uint32_t _h = *(uint32_t *)((char *)_vio_state_{0} + {1});\n"
            "        uint32_t _t = *(uint32_t *)((char *)_vio_state_{0} + {2});\n"
            "        if (_h != _t) _qv->phase = {3};\n",
            s.id, head_off, tail_off, Q_AVAIL_IDX);
    // 装不下一个事件（< 8 字节）的 buffer 直接以 len=0 完成，不能让队列卡住
    q.desc = std::format("                _qv->phase = _qv->dlen < 8 ? {0} : {1};\n", Q_USED, Q_DEV);
    q.start = std::format(
            "        case {0}: {{\n"
            "            uint32_t _h = *(uint32_t *)((char *)_vio_state_{1} + {2});\n"
            "            uint32_t _t = *(uint32_t *)((char *)_vio_state_{1} + {3});\n"
            "            uint32_t _n = _h - _t;\n"
            "            uint32_t _cap = _qv->dlen / 8;\n"
            "            if (_n > _cap) _n = _cap;\n"
            "            if (_n > 8) _n = 8;\n"
            "            if (_n == 0) break; // 暂时没有事件：保持阶段，等宿主 push\n"
            "            _qv->step = _t;\n"
            "{4}"
            "            _qv->chunk = _n * 8;\n"
            "            _a = _qv->daddr; _l = _qv->chunk; _wr = 1;\n"
            "            break;\n"
            "        }}\n",
            Q_DEV, s.id, head_off, tail_off, copy_events);
    q.done = std::format(
            "            case {0}: {{ // 事件块写完\n"
            "                *(uint32_t *)((char *)_vio_state_{1} + {2}) = _qv->step + _qv->chunk / 8;\n"
            "                _qv->written = _qv->chunk;\n"
            "                _qv->phase = {3};\n"
            "                break;\n"
            "            }}\n",
            Q_DEV, s.id, tail_off, Q_USED);
    s.queues.push_back(q);
    return s;
}

std::string VirtioInput::genStateDecl() const {
    return virtio_gen::genStateDecl(makeSpec());
}

std::string VirtioInput::genFuncDef_seq() const {
    return virtio_gen::genSeq(makeSpec());
}

std::string VirtioInput::genInitCode() const {
    return virtio_gen::genInitCode(makeSpec(), _state.get());
}

std::unique_ptr<Component> VirtioInput::clone(const std::string &new_name) const {
    return std::make_unique<VirtioInput>(new_name);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_input", {},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-input：宿主 pushInputEvent 塞 evdev 事件，设备把事件写进驱动提交的缓冲区"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::VirtioInput>(n);
    });
} // namespace
