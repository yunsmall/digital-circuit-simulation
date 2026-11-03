#include "dcs/components/platform/VirtioRng.h"

#include "dcs/ComponentFactory.h"

#include <format>

namespace dsc {

struct VirtioRng::State {
    BackendOps ops;
};

namespace {
// 生成代码里调用的复位回调名。JIT 符号表按名字索引，必须和 register 时用的字符串一致
constexpr const char *kResetSymbol = "dcs_virtio_rng_reset";

void resetThunk(void *ctx) {
    auto *s = static_cast<VirtioRng::State *>(ctx);
    if (s->ops.reset)
        s->ops.reset(s->ops.ctx);
}
} // namespace

VirtioRng::VirtioRng(const std::string &name) : SequentialComponent(name, "virtio_rng"), _state(std::make_unique<State>()) {
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

VirtioRng::~VirtioRng() = default;

void VirtioRng::setBackend(const BackendOps &ops) {
    _state->ops = ops;
}

void VirtioRng::simInit() {
    if (_state->ops.init)
        _state->ops.init(_state->ops.ctx);
}

void VirtioRng::simDeinit() {
    if (_state->ops.deinit)
        _state->ops.deinit(_state->ops.ctx);
}

std::vector<Component::JitSymbol> VirtioRng::extraJitSymbols() const {
    return {{kResetSymbol, reinterpret_cast<void *>(&resetThunk)},
            {"dcs_virtio_trace", reinterpret_cast<void *>(&dcs_virtio_trace)}};
}

virtio_gen::DeviceSpec VirtioRng::makeSpec() const {
    using namespace virtio_gen;
    DeviceSpec s;
    s.id = std::to_string(_id);
    s.func_name = funcName_seq();
    s.node_name = std::format("VirtioRng {}", name());
    s.reset_symbol = kResetSymbol;
    s.dev_id = 4;
    s.nq = 1;
    s.features = 0;
    for (int i = 0; i < virtio_gen::IN_COUNT; i++)
        s.in_nets[i] = (int) inputs()[i]->netId();
    s.state_decl = std::format("static uint64_t _vio_rng_{} = 0x9E3779B97F4A7C15ULL;\n", s.id);
    s.config_read = "";
    s.out_write = [this](int i, const std::string &e, uint32_t w) { return genOutputWrite(i, e, w); };

    // 生成期展开 8 组：每拍最多搬 64 字节，够写满缓冲区
    std::string fill;
    for (int k = 0; k < 8; k++) {
        fill += "            _r ^= _r << 13; _r ^= _r >> 7; _r ^= _r << 17;\n";
        fill += std::format("            dcs_memcpy(_vio_mp_buf_{} + {}, &_r, 8);\n", s.id, k * 8);
    }

    QueueSpec q;
    q.wake = "";
    q.desc = std::format("                _qv->phase = _qv->dlen == 0 ? {0} : {1};\n", Q_USED, Q_DEV);
    q.start = std::format(
            "        case {0}: {{\n"
            "            uint32_t _c = _qv->dlen - _qv->off;\n"
            "            if (_c > 64) _c = 64;\n"
            "            _qv->chunk = _c;\n"
            "            uint64_t _r = _vio_rng_{1};\n"
            "{2}"
            "            _vio_rng_{1} = _r;\n"
            "            _a = _qv->daddr + _qv->off; _l = _c; _wr = 1;\n"
            "            break;\n"
            "        }}\n",
            Q_DEV, s.id, fill);
    q.done = std::format(
            "            case {0}: {{\n"
            "                _qv->off += _qv->chunk;\n"
            "                _qv->written += _qv->chunk;\n"
            "                if (_qv->off >= _qv->dlen) {{\n"
            "                    if (_qv->dflags & 1) {{ _qv->idx = _qv->dnext; _qv->phase = {1}; }}\n"
            "                    else _qv->phase = {2};\n"
            "                }}\n"
            "                break;\n"
            "            }}\n",
            Q_DEV, Q_DESC, Q_USED);
    s.queues.push_back(q);
    return s;
}

std::string VirtioRng::genStateDecl() const {
    return virtio_gen::genStateDecl(makeSpec());
}

std::string VirtioRng::genFuncDef_seq() const {
    return virtio_gen::genSeq(makeSpec());
}

std::string VirtioRng::genInitCode() const {
    return virtio_gen::genInitCode(makeSpec(), _state.get());
}

std::unique_ptr<Component> VirtioRng::clone(const std::string &new_name) const {
    return std::make_unique<VirtioRng>(new_name);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_rng", {},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-rng：设备把随机数写进驱动提供的缓冲区，随机数在 JIT 里生成"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::VirtioRng>(n);
    });
} // namespace
