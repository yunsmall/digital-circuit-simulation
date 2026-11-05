#include "dcs/components/platform/VirtioInput.h"

#include "dcs/ComponentFactory.h"

#include <atomic>
#include <cstddef>
#include <cstring>
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
    // guest 通过 status 队列回传的事件（LED 状态等），设备写、宿主读
    InputEvent status[EVT_RING] = {};
    uint32_t status_head = 0;
    uint32_t status_tail = 0;

    // config 空间的只读载荷。Linux 是逐字节读 u.string/u.bitmap 的，
    // 生成代码按 (select, subsel) 挑一个数组再按字节搬
    uint8_t cfg_name[128] = {};
    uint8_t cfg_devids[8] = {};
    uint8_t cfg_keybits[128] = {};
    uint8_t cfg_ledbits[128] = {};

    State() {
        const char *n = "dcs-virtual-input";
        std::memcpy(cfg_name, n, std::strlen(n));
        // virtio_input_devids：bustype/vendor/product/version，全小端
        cfg_devids[0] = 0x06; // BUS_VIRTUAL
        cfg_devids[2] = 0x43; // 'D'
        cfg_devids[3] = 0x44; // 'C'
        cfg_devids[4] = 0x01;
        cfg_devids[6] = 0x01;
        // 键盘能力：ESC(1)、ENTER(28)、A..L(30..38)、SPACE(57)，位图首字节是 bit0..7
        cfg_keybits[1] |= 0x02; // 1
        cfg_keybits[3] |= 0x10; // 28
        cfg_keybits[3] |= 0x40; // 30
        cfg_keybits[3] |= 0x80; // 31
        cfg_keybits[4] = 0x7F;  // 32..38
        cfg_keybits[7] |= 0x02; // 57
        // LED：NUML(0)、CAPSL(1)、SCROLLL(2)
        cfg_ledbits[0] = 0x07;
    }
};

namespace {
// 生成代码里调用的复位回调名。JIT 符号表按名字索引，必须和 register 时用的字符串一致
constexpr const char *kResetSymbol = "dcs_virtio_input_reset";

void resetThunk(void *ctx) {
    auto *s = static_cast<VirtioInput::State *>(ctx);
    // 后端创建失败时 ctx 为空，不能指望后端实现自己防御
    if (s->ops.reset && s->ops.ctx)
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
    // 重复 init 不能再清环、再调后端 init：后端 init 不可重入，而且宿主在两次
    // init 之间可能已经 push 了事件。deinit 会清掉标志，下一轮才重新执行
    if (_init_called)
        return;
    _init_called = true;
    // 重新 init 是冷启动：上一轮残留的输入事件/状态回传不能投递给新 guest。
    // 旧后端的线程在 simDeinit 里已经停掉，这里没有并发访问
    _state->evt_head = _state->evt_tail = 0;
    _state->status_head = _state->status_tail = 0;
    if (_state->ops.init)
        _state->ops.init(_state->ops.ctx);
}

void VirtioInput::simDeinit() {
    _init_called = false;
    // clear()/析构会重复调用 simDeinit；deinit 后把 ctx 置空，避免后端回调被调两次
    if (_state->ops.deinit && _state->ops.ctx) {
        _state->ops.deinit(_state->ops.ctx);
        _state->ops.ctx = nullptr;
    }
}

std::vector<Component::JitSymbol> VirtioInput::extraJitSymbols() const {
    return {{kResetSymbol, reinterpret_cast<void *>(&resetThunk)},
            {"dcs_virtio_trace", reinterpret_cast<void *>(&dcs_virtio_trace)}};
}

bool VirtioInput::pushInputEvent(const InputEvent &ev) {
    auto &s = *_state;
    // head/tail 都用实际增长值，只在索引时取模。若 head 写成模值而 tail 单调增长，
    // tail 越过 RING 后 (head+1)%RING == tail 永远不成立，环满检测失效，
    // q.start 里的 _h - _t 也会下溢成巨值。
    // 宿主线程写、JIT 线程读，必须走原子：先写事件再 release 发布 head，
    // 否则弱内存序下 JIT 可能先看到 head 前进、读到还没写完的事件
    const uint32_t head = std::atomic_ref<uint32_t>(s.evt_head).load(std::memory_order_relaxed);
    const uint32_t tail = std::atomic_ref<uint32_t>(s.evt_tail).load(std::memory_order_acquire);
    // 用差值判满而不是 head+1 取模：head 到 UINT32_MAX 后自增回绕，
    // 取模比较会在 tail 恰好对齐时误判成满；差值按无符号回绕永远正确
    const uint32_t used = head - tail;
    if (used >= State::EVT_RING - 1)
        return false; // 环满（留一个空槽区分满和空）
    s.events[head % State::EVT_RING] = ev;
    std::atomic_ref<uint32_t>(s.evt_head).store(head + 1, std::memory_order_release);
    return true;
}

bool VirtioInput::popStatusEvent(InputEvent &ev) {
    auto &s = *_state;
    // guest 侧先写 status 槽再 release 发布 status_head，这里 acquire 后读才安全
    const uint32_t head = std::atomic_ref<uint32_t>(s.status_head).load(std::memory_order_acquire);
    const uint32_t tail = std::atomic_ref<uint32_t>(s.status_tail).load(std::memory_order_relaxed);
    if (head == tail)
        return false;
    ev = s.status[tail % State::EVT_RING];
    std::atomic_ref<uint32_t>(s.status_tail).store(tail + 1, std::memory_order_release);
    return true;
}

virtio_gen::DeviceSpec VirtioInput::makeSpec() const {
    using namespace virtio_gen;
    DeviceSpec s;
    s.id = std::to_string(_id);
    s.func_name = funcName_seq();
    s.node_name = std::format("VirtioInput {}", name());
    s.reset_symbol = kResetSymbol;
    s.dev_id = 18;
    // Linux 驱动固定开两条队列：0 = events（驱动补 buffer），1 = status（guest 回传 LED 等）。
    // 少给一条时驱动给队列 1 写 QUEUE_SEL 会被钳到队列 0，读回 QUEUE_READY 非 0，
    // probe 直接以 -ENOENT 失败
    s.nq = 2;
    s.features = 0;
    for (int i = 0; i < virtio_gen::IN_COUNT; i++)
        s.in_nets[i] = (int) inputs()[i]->netId();

    const unsigned long long head_off = offsetof(State, evt_head);
    const unsigned long long tail_off = offsetof(State, evt_tail);
    const unsigned long long evt_off = offsetof(State, events);
    const unsigned long long st_head_off = offsetof(State, status_head);
    const unsigned long long st_tail_off = offsetof(State, status_tail);
    const unsigned long long st_off = offsetof(State, status);
    const unsigned long long name_off = offsetof(State, cfg_name);
    const unsigned long long devids_off = offsetof(State, cfg_devids);
    const unsigned long long keybits_off = offsetof(State, cfg_keybits);
    const unsigned long long ledbits_off = offsetof(State, cfg_ledbits);
    const unsigned ring = State::EVT_RING;

    s.state_decl = std::format("static uint32_t _vio_icfg_sel_{0}, _vio_icfg_sub_{0};\n", s.id, s.id);
    s.init_code = std::format("    _vio_icfg_sel_{0} = 0; _vio_icfg_sub_{0} = 0;\n", s.id);
    s.out_write = [this](int i, const std::string &e, uint32_t w) { return genOutputWrite(i, e, w); };

    // config 空间：0x100 是 select/subsel/size 三个字节，0x108 起是 128 字节载荷。
    // 载荷按 (select, subsel) 从宿主侧数组里取，生成代码只做一次指针选择 + 4 字节拷贝。
    // 注意 _off 是 Wishbone 字地址（字对齐），驱动对 0x101/0x102 的单字节读会落在
    // 0x100 这个 case 上，由 CPU 按字节偏移从返回的 32 位字里取；不要拆成 0x101/0x102 的 case
    std::string payload_cases;
    for (int k = 0; k < 32; k++)
        payload_cases += std::format("            case 0x{:X}: ", 0x108 + k * 4);

    s.config_read = std::format(
            "            case 0x100: {{\n"
            "                _dout = _vio_icfg_sel_{0} | (_vio_icfg_sub_{0} << 8);\n"
            // size 字节在这里填：ID_NAME=17、ID_DEVIDS=8、EV_KEY 位图 8 字节、EV_LED 位图 4 字节
            "                if (_vio_icfg_sel_{0} == 1) _dout |= 17u << 16;\n"
            "                else if (_vio_icfg_sel_{0} == 3) _dout |= 8u << 16;\n"
            "                else if (_vio_icfg_sel_{0} == 0x11 && _vio_icfg_sub_{0} == 1) _dout |= 8u << 16;\n"
            "                else if (_vio_icfg_sel_{0} == 0x11 && _vio_icfg_sub_{0} == 0x11) _dout |= 4u << 16;\n"
            "                break;\n"
            "            }}\n"
            "{1}"
            "            {{\n"
            "                const uint32_t _b = _off - 0x108u;\n"
            "                const uint8_t *_p = 0;\n"
            // 每项载荷都有自己的长度：ID_DEVIDS 只有 8 字节，驱动按 0x108+4k
            // 读到后面会穿越到相邻的 cfg_keybits，必须按 size 截断并补零
            "                uint32_t _sz = 0;\n"
            "                if (_vio_icfg_sel_{0} == 1) {{ _p = (const uint8_t *)_vio_state_{0} + {2}; _sz = 17; }}\n"
            "                else if (_vio_icfg_sel_{0} == 3) {{ _p = (const uint8_t *)_vio_state_{0} + {3}; _sz = 8; }}\n"
            "                else if (_vio_icfg_sel_{0} == 0x11 && _vio_icfg_sub_{0} == 1)\n"
            "                    {{ _p = (const uint8_t *)_vio_state_{0} + {4}; _sz = 8; }}\n"
            "                else if (_vio_icfg_sel_{0} == 0x11 && _vio_icfg_sub_{0} == 0x11)\n"
            "                    {{ _p = (const uint8_t *)_vio_state_{0} + {5}; _sz = 4; }}\n"
            "                _dout = 0;\n"
            "                if (_p && _b < _sz) {{\n"
            "                    uint32_t _cn = _sz - _b;\n"
            "                    if (_cn > 4) _cn = 4;\n"
            "                    dcs_memcpy(&_dout, _p + _b, _cn);\n"
            "                }}\n"
            "                break;\n"
            "            }}\n",
            s.id, payload_cases, name_off, devids_off, keybits_off, ledbits_off);

    // 写 select/subsel 都是单字节事务，总线地址仍是字对齐的 0x100，
    // 具体写的哪个字节由 _sel 的位给出（0x101 的写是 _sel bit1，不是 _off=0x101）
    s.config_write = std::format(
            "            case 0x100: {{\n"
            "                if (_sel & 1u) _vio_icfg_sel_{0} = _wdat & 0xFFu;\n"
            "                if (_sel & 2u) _vio_icfg_sub_{0} = (_wdat >> 8) & 0xFFu;\n"
            "                break;\n"
            "            }}\n",
            s.id);

    // 生成期展开 8 个事件槽：常量下标，没有循环
    std::string copy_events;
    for (int k = 0; k < 8; k++)
        copy_events += std::format(
                "            if ({0}u < _n) dcs_memcpy(_vio_mp_buf_{2} + {1}, "
                "(char *)_vio_state_{2} + {3} + ((_t + {0}u) % {4}u) * 8, 8);\n",
                k, k * 8, s.id, evt_off, ring);

    QueueSpec q;
    q.wake = std::format(
            "        uint32_t _h = dcs_load_acq((char *)_vio_state_{0} + {1});\n"
            "        uint32_t _t = dcs_load_acq((char *)_vio_state_{0} + {2});\n"
            "        if (_h != _t) _qv->phase = {3};\n",
            s.id, head_off, tail_off, Q_AVAIL_IDX);
    // 装不下一个事件（< 8 字节）的 buffer 直接以 len=0 完成，不能让队列卡住
    q.desc = std::format("                _qv->phase = _qv->dlen < 8 ? {0} : {1};\n", Q_USED, Q_DEV);
    q.start = std::format(
            "        case {0}: {{\n"
            "            uint32_t _h = dcs_load_acq((char *)_vio_state_{1} + {2});\n"
            "            uint32_t _t = dcs_load_acq((char *)_vio_state_{1} + {3});\n"
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
            "                dcs_store_rel((char *)_vio_state_{1} + {2}, _qv->step + _qv->chunk / 8);\n"
            "                _qv->written = _qv->chunk;\n"
            "                _qv->phase = {3};\n"
            "                break;\n"
            "            }}\n",
            Q_DEV, s.id, tail_off, Q_USED);
    // 驱动提交的是事件缓冲区，notify 只能唤醒队列让 wake 去查事件环，
    // 不能直接把每个空 buffer 都当请求处理掉
    q.notify_kick = false;
    s.queues.push_back(q);

    // status 队列：guest 发来的 LED/状态事件是单描述符 out buffer，读 8 字节落到宿主环
    QueueSpec st;
    st.wake = "";
    st.desc = std::format(
            "                if (_qv->dflags & 1) {{ _qv->idx = _qv->dnext; _qv->phase = {0}; }}\n"
            "                else _qv->phase = _qv->dlen < 8 ? {1} : {2};\n",
            Q_DESC, Q_USED, Q_DEV + 2);
    st.start = std::format("        case {0}: _a = _qv->daddr; _l = 8; _wr = 0; break;\n", Q_DEV + 2);
    st.done = std::format(
            "            case {0}: {{\n"
            "                uint32_t _sh = dcs_load_acq((char *)_vio_state_{1} + {2});\n"
            "                uint32_t _stt = dcs_load_acq((char *)_vio_state_{1} + {3});\n"
            "                if (_sh - _stt < {4}) {{\n"
            "                    dcs_memcpy((char *)_vio_state_{1} + {5} + (_sh % {4}) * 8, _vio_mp_buf_{1}, 8);\n"
            "                    dcs_store_rel((char *)_vio_state_{1} + {2}, _sh + 1);\n"
            "                }}\n"
            "                _qv->written = 0;\n"
            "                _qv->phase = {6};\n"
            "                break;\n"
            "            }}\n",
            Q_DEV + 2, s.id, st_head_off, st_tail_off, ring, st_off, Q_USED);
    s.queues.push_back(st);
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
    // 宿主注入的后端不复制：clone 用于复合元件展开，复制会让两个实例共享
    // 同一份后端上下文并在销毁时重复释放
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
