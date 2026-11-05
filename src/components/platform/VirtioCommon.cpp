#include "dcs/components/platform/VirtioCommon.h"
#include "dcs/CodeGenText.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

namespace dsc::virtio_gen {

std::string qv(const std::string &id, int q, const char *field) {
    return std::format("_vio_q_{0}[{1}].{2}", id, q, field);
}

std::string genStateDecl(const DeviceSpec &s) {
    return std::format(
                   "#define _VIO_{0}_DEVID {1}u\n"
                   "#define _VIO_{0}_NQ {2}u\n"
                   "#define _VIO_{0}_QUEUE_MAX {3}u\n"
                   "typedef struct {{\n"
                   "    uint32_t desc, avail, used, qnum, qready, last_avail, used_idx;\n"
                   "    uint32_t head, idx, daddr, dlen, dnext, off, written, step, chunk, pkt;\n"
                   "    uint16_t dflags; uint8_t phase;\n"
                   "}} _VioQ_{0};\n"
                   "static _VioQ_{0} _vio_q_{0}[{2}];\n"
                   "static uint32_t _vio_status_{0}, _vio_int_{0}, _vio_qsel_{0}, _vio_dsel_{0}, _vio_rsel_{0};\n"
                   "static uint8_t _vio_prev_stb_{0};\n"
                   // 主端口/队列阶段卡死检测：连续这么多拍没进展就报一次，方便定位
                   "static uint32_t _vio_wd_{0}, _vio_pd_{0};\n"
                   "extern void dcs_virtio_trace(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n"
                   "static uint8_t _vio_mp_active_{0}, _vio_mp_write_{0}, _vio_mp_ackp_{0}, _vio_mp_gap_{0}, _vio_mp_q_{0};\n"
                   "static uint32_t _vio_mp_addr_{0}, _vio_mp_len_{0}, _vio_mp_beat_{0};\n"
                   "static uint8_t _vio_mp_buf_{0}[64];\n"
                   "static void *_vio_state_{0};\n"
                   // 复位回调按设备类取唯一名字：JIT 的符号表是按名字覆盖的，
                   // 几个 virtio 设备共用 dcs_virtio_reset 时，最后一次注册会把别的
                   // 设备的 thunk 顶替掉，再拿对方的状态指针当自己的 State 解释（宿主崩溃）
                   "extern void {4}(void *);\n",
                   s.id, s.dev_id, s.nq, QUEUE_MAX, s.reset_symbol) +
           s.state_decl;
}

std::string genInitCode(const DeviceSpec &s, void *state) {
    return std::format(
                   "    _vio_status_{0} = 0; _vio_int_{0} = 0; _vio_qsel_{0} = 0; _vio_dsel_{0} = 0; _vio_rsel_{0} = 0;\n"
                   "    _vio_prev_stb_{0} = 0; _vio_mp_active_{0} = 0; _vio_mp_gap_{0} = 0; _vio_mp_q_{0} = 0;\n"
                   "    _vio_wd_{0} = 0; _vio_pd_{0} = 0;\n"
                   "    dcs_memset(_vio_q_{0}, 0, sizeof(_vio_q_{0}));\n"
                   "    _vio_state_{0} = (void *)0x{1:X}ULL;\n",
                   s.id, (unsigned long long) reinterpret_cast<uintptr_t>(state)) +
           s.init_code;
}

namespace {

std::string genMmio(const DeviceSpec &s) {
    // 只有需要"notify 即处理"的队列才在 kick 时启动；RX 队列交给每拍的 wake 检查，
    // 否则驱动每次补 buffer 的 notify 都会让设备消费一个空描述符
    std::string kick_code;
    for (int q = 0; q < (int) s.nq; q++) {
        if (!s.queues[q].notify_kick)
            continue;
        kick_code += std::format(
                // 驱动在设备处理中途再写 notify（连续发包时很常见）不能把当前阶段拽回
                // 开头：在途的主端口事务会按新阶段解释自己搬回的数据。只允许空闲时启动，
                // 已排队的新请求由阶段 8 回到阶段 1 时自然看到
                "                    if (_qi == {0}u && _vio_q_{1}[{0}].phase == 0)\n"
                "                        _vio_q_{1}[{0}].phase = _vio_q_{1}[{0}].qready ? 1 : 0;\n", q,
                s.id);
    }
    return std::format(
            "    // ---- 从端口：virtio-mmio 寄存器 ----\n"
            "    if (_active) {{\n"
            "        const uint32_t _off = _adr & 0xFFFu;\n"
            "        if (!_we) {{\n"
            "            switch (_off) {{\n"
            "            case 0x000: _dout = 0x74726976u; break;\n"
            "            case 0x004: _dout = 2; break;\n"
            "            case 0x008: _dout = _VIO_{0}_DEVID; break;\n"
            "            case 0x00C: _dout = 0x444353u; break;\n"
            "            case 0x010:\n"
            "                if (_vio_dsel_{0} == 0) _dout = {1}u;\n"
            "                else if (_vio_dsel_{0} == 1) _dout = 1u;\n"
            "                break;\n"
            "            case 0x034: _dout = _VIO_{0}_QUEUE_MAX; break;\n"
            "            case 0x044: _dout = _vio_q_{0}[_vio_qsel_{0}].qready; break;\n"
            "            case 0x060: _dout = _vio_int_{0}; break;\n"
            "            case 0x070: _dout = _vio_status_{0}; break;\n"
            "            case 0x0FC: _dout = 0; break;\n"
            "{2}"
            "            default: _dout = 0; break;\n"
            "            }}\n"
            "        }} else if (_rise) {{\n"
            "            switch (_off) {{\n"
            "            case 0x014: _vio_dsel_{0} = _wdat & 1; break;\n"
            "            case 0x024: _vio_rsel_{0} = _wdat & 1; break;\n"
            "            case 0x030: _vio_qsel_{0} = _wdat < _VIO_{0}_NQ ? _wdat : _VIO_{0}_NQ - 1; break;\n"
            "            case 0x038: {{\n"
            "                _VioQ_{0} *_q = &_vio_q_{0}[_vio_qsel_{0}];\n"
            // QueueNum 是环形缓冲的除数，0 会让后面的 % 直接除零。驱动不写就想
            // ready 时按 1 处理，写 0 也钳到 1
            "                _q->qnum = _wdat == 0 ? 1u : (_wdat > _VIO_{0}_QUEUE_MAX ? _VIO_{0}_QUEUE_MAX : _wdat);\n"
            "                break;\n"
            "            }}\n"
            "            case 0x044: {{\n"
            "                _VioQ_{0} *_q = &_vio_q_{0}[_vio_qsel_{0}];\n"
            "                if (_q->qnum == 0) _q->qnum = 1;\n"
            "                _q->qready = _wdat & 1;\n"
            "                _q->last_avail = 0; _q->used_idx = 0; _q->phase = 0;\n"
            "                break;\n"
            "            }}\n"
            "            case 0x050: {{\n"
            // 队列没 ready 时的 notify 必须忽略，生成代码里每个队列单独判断 qready
            "                const uint32_t _qi = _wdat;\n"
            "                if (_qi < _VIO_{0}_NQ) {{\n"
            "                    dcs_virtio_trace({0}u, 0x301u, _qi, _vio_q_{0}[_qi].qready, _vio_q_{0}[_qi].qnum, "
            "_vio_q_{0}[_qi].avail, _vio_q_{0}[_qi].desc, 0);\n"
            "{4}"
            "                }}\n"
            "                break;\n"
            "            }}\n"
            "            case 0x064: _vio_int_{0} &= ~_wdat; break;\n"
            "            case 0x070:\n"
            "                _vio_status_{0} = _wdat;\n"
            "                dcs_virtio_trace({0}u, 0x302u, _wdat, _vio_q_{0}[0].qready, _vio_q_{0}[0].qnum, 0, 0, 0);\n"
            "                if (_wdat == 0) {{\n"
            "                    _vio_int_{0} = 0;\n"
            "                    dcs_memset(_vio_q_{0}, 0, sizeof(_vio_q_{0}));\n"
            // 复位时主端口可能正有事务在途：不清掉的话下一拍还会按旧地址继续搬数据，
            // 完成回调又会拿着已清零的队列推进阶段
            "                    _vio_mp_active_{0} = 0; _vio_mp_gap_{0} = 0; _vio_mp_q_{0} = 0;\n"
            "                    _vio_mp_ackp_{0} = 0; _vio_mp_beat_{0} = 0; _vio_mp_write_{0} = 0;\n"
            "                    _vio_wd_{0} = 0; _vio_pd_{0} = 0;\n"
            "                    {6}(_vio_state_{0});\n"
            "                }}\n"
            "                break;\n"
            // virtio-mmio 的 *Low/*High 各是 32 位寄存器，High 存地址的高 32 位。
            // guest 物理地址都在 32 位以内，High 写 0 时直接忽略，不能拿它当高 16 位拼
            "            case 0x080: {{ _VioQ_{0} *_q = &_vio_q_{0}[_vio_qsel_{0}]; _q->desc = _wdat; break; }}\n"
            "            case 0x084: break;\n"
            "            case 0x090: {{ _VioQ_{0} *_q = &_vio_q_{0}[_vio_qsel_{0}]; _q->avail = _wdat; break; }}\n"
            "            case 0x094: break;\n"
            "            case 0x0A0: {{ _VioQ_{0} *_q = &_vio_q_{0}[_vio_qsel_{0}]; _q->used = _wdat; break; }}\n"
            "            case 0x0A4: break;\n"
            "            case 0x020: break;\n"
            "{5}"
            "            default: break;\n"
            "            }}\n"
            "            dcs_virtio_trace({0}u, 0x305u, _off, _wdat, _vio_qsel_{0}, _vio_q_{0}[0].qready, {3}, 0);\n"
            "        }}\n"
            "        _ack = 1;\n"
            "    }}\n",
            s.id, s.features, s.config_read,
            s.nq >= 2 ? std::format("_vio_q_{}[1].qready", s.id) : std::string("0u"), kick_code, s.config_write,
            s.reset_symbol);
}

std::string genMaster(const DeviceSpec &s) {
    return std::format(
            "    // ---- 主端口事务：每拍最多 4 字节，按字节使能处理非对齐访问 ----\n"
            "    // 从设备在 stb 上升沿处理事务，每搬完一个 beat 要撤销一拍 stb\n"
            "    uint32_t _m_adr = 0, _m_dout = 0;\n"
            "    uint8_t _m_we = 0, _m_stb = 0, _m_cyc = 0, _mp_done = 0;\n"
            "    uint8_t _m_sel = 0, _mp_gap_now = 0;\n"
            "    if (_vio_mp_active_{0}) {{\n"
            "        if (_vio_mp_gap_{0}) {{\n"
            "            _vio_mp_gap_{0} = 0; _vio_mp_ackp_{0} = 0; _mp_gap_now = 1;\n"
            "        }} else {{\n"
            "            uint32_t _step = _vio_mp_len_{0} - _vio_mp_beat_{0};\n"
            "            if (_step > 4) _step = 4;\n"
            "            const uint32_t _ma = _vio_mp_addr_{0} + _vio_mp_beat_{0};\n"
            // Wishbone 的 SEL 只有 4 位、单拍不能跨字：地址非对齐时本拍长度截到字尾
            "            const uint32_t _room = 4u - (_ma & 3u);\n"
            "            if (_step > _room) _step = _room;\n"
            "            const uint32_t _shift = (_ma & 3) * 8;\n"
            // 32 位字节粒度总线的 ADR 是字地址，字节由 SEL 选
            "            _m_adr = _ma & ~3u; _m_stb = 1; _m_cyc = 1; _m_we = _vio_mp_write_{0};\n"
            "            _m_sel = (uint8_t)(((1u << _step) - 1u) << (_ma & 3));\n"
            "            if (_vio_mp_write_{0}) {{\n"
            // 只搬本拍实际长度：beat 到 63 时读 4 字节会越出 64 字节缓冲
            "                uint32_t _v = 0; dcs_memcpy(&_v, _vio_mp_buf_{0} + _vio_mp_beat_{0}, _step);\n"
            "                _m_dout = _v << _shift;\n"
            "            }}\n"
            "            if (_m_ack && !_vio_mp_ackp_{0}) {{\n"
            "                if (!_vio_mp_write_{0}) {{\n"
            "                    uint32_t _v = _m_dat >> _shift;\n"
            "                    dcs_memcpy(_vio_mp_buf_{0} + _vio_mp_beat_{0}, &_v, _step);\n"
            "                }}\n"
            "                _vio_mp_beat_{0} += _step;\n"
            "                if (_vio_mp_beat_{0} >= _vio_mp_len_{0}) {{ _vio_mp_active_{0} = 0; _mp_done = 1; }}\n"
            "                _vio_mp_gap_{0} = 1;\n"
            "            }}\n"
            "            _vio_mp_ackp_{0} = _m_ack;\n"
            "        }}\n"
            "    }} else if (_vio_mp_gap_{0}) {{\n"
            "        _vio_mp_gap_{0} = 0; _vio_mp_ackp_{0} = 0; _mp_gap_now = 1;\n"
            "    }}\n",
            s.id);
}

std::string genDone(const DeviceSpec &s) {
    std::string c;
    c += std::format("    // ---- virtqueue 完成处理 ----\n"
                     "    if (_mp_done && _vio_mp_q_{0} < _VIO_{0}_NQ) {{\n",
                     s.id);
    c += std::format("        _VioQ_{0} *_qv = &_vio_q_{0}[_vio_mp_q_{0}];\n", s.id);
    c += std::format("        switch (_vio_mp_q_{0}) {{\n", s.id);
    for (int q = 0; q < (int) s.nq; q++) {
        c += std::format("        case {}: {{\n", q);
        c += "            switch (_qv->phase) {\n";
        c += "            case 1: { // avail.idx\n";
        c += std::format("                uint32_t _i = (uint32_t)_vio_mp_buf_{0}[0] | ((uint32_t)_vio_mp_buf_{0}[1] << 8);\n", s.id);
        // avail.idx 是规范的 16 位回绕计数，内部 last_avail 是 32 位。只比低 16 位，
        // 否则队列跑过 65536 个请求后两边永远不等
        c += "                _qv->phase = _i == (uint16_t)_qv->last_avail ? 0 : 2;\n";
        c += "                break;\n";
        c += "            }\n";
        c += "            case 2: { // avail.ring\n";
        c += std::format("                _qv->head = (uint32_t)_vio_mp_buf_{0}[0] | ((uint32_t)_vio_mp_buf_{0}[1] << 8);\n", s.id);
        c += "                _qv->idx = _qv->head;\n";
        // 每个请求在进 desc 链之前清零 written：desc 处理里“dlen 不足直接完成”的
        // 分支不会再写它，used.len 必须报 0，不能带上一个请求的字节数
        c += "                _qv->written = 0; _qv->pkt = 0; _qv->step = 0;\n";
        c += "                _qv->phase = 3;\n";
        c += "                break;\n";
        c += "            }\n";
        c += "            case 3: { // descriptor\n";
        c += std::format("                uint64_t _addr = 0; dcs_memcpy(&_addr, _vio_mp_buf_{0}, 8);\n", s.id);
        c += "                _qv->daddr = (uint32_t)_addr;\n";
        c += std::format("                _qv->dlen = (uint32_t)_vio_mp_buf_{0}[8] | ((uint32_t)_vio_mp_buf_{0}[9] << 8) |\n", s.id);
        c += std::format("                            ((uint32_t)_vio_mp_buf_{0}[10] << 16) | ((uint32_t)_vio_mp_buf_{0}[11] << 24);\n", s.id);
        c += std::format("                _qv->dflags = (uint16_t)((uint32_t)_vio_mp_buf_{0}[12] | ((uint32_t)_vio_mp_buf_{0}[13] << 8));\n", s.id);
        c += std::format("                _qv->dnext = (uint32_t)_vio_mp_buf_{0}[14] | ((uint32_t)_vio_mp_buf_{0}[15] << 8);\n", s.id);
        c += "                _qv->off = 0;\n";
        c += std::format(
                "                dcs_virtio_trace({0}u, 0x304u, {1}u, _qv->daddr, _qv->dlen, (uint32_t) _qv->dflags, "
                "_qv->dnext, 0);\n",
                s.id, q);
        c += s.queues[q].desc;
        c += "                break;\n";
        c += "            }\n";
        c += "            case 7: { // used.ring 元素写完\n";
        c += "                _qv->phase = 8;\n";
        c += "                break;\n";
        c += "            }\n";
        c += "            case 8: { // used.idx 写完，继续看 avail 里还有没有请求\n";
        c += "                _qv->used_idx++; _qv->last_avail++;\n";
        c += std::format("                _vio_int_{} |= 1u;\n", s.id);
        c += std::format(
                "                dcs_virtio_trace({0}u, 0x303u, _vio_mp_q_{0}, _qv->used_idx, _qv->last_avail, "
                "_qv->head, _qv->written, 0);\n",
                s.id);
        c += "                _qv->phase = 1;\n";
        c += "                break;\n";
        c += "            }\n";
        c += s.queues[q].done;
        c += "            default: _qv->phase = 0; break;\n";
        c += "            }\n";
        c += "            break;\n";
        c += "        }\n";
    }
    c += "        default: break;\n";
    c += "        }\n";
    c += "    }\n";
    return c;
}

// 队列空闲时的宿主侧唤醒检查。必须每拍都跑（网卡的收包环、input 的事件环都是
// 异步到达的），但它只碰两个宿主计数器和队列状态，不碰总线输入，所以放在
// 空闲快速路径里面
std::string genWake(const DeviceSpec &s, int q) {
    std::string c;
    c += std::format("    if (_vio_q_{0}[{1}].phase == 0 && _vio_q_{0}[{1}].qready) {{\n", s.id, q);
    c += std::format("        _VioQ_{0} *_qv = &_vio_q_{0}[{1}];\n", s.id, q);
    c += "        (void) _qv;\n";
    c += s.queues[q].wake;
    c += "    }\n";
    return c;
}

std::string genQueue(const DeviceSpec &s, int q) {
    std::string c;
    c += std::format("    // ---- 队列 {} ----\n", q);
    c += std::format("    if (!_vio_mp_active_{0} && !_mp_gap_now && _vio_q_{0}[{1}].phase != 0) {{\n", s.id, q);
    c += std::format("        _VioQ_{0} *_qv = &_vio_q_{0}[{1}];\n", s.id, q);
    c += "        uint32_t _a = 0, _l = 0; uint8_t _wr = 0;\n";
    c += "        switch (_qv->phase) {\n";
    c += "        case 1: _a = _qv->avail + 2; _l = 2; _wr = 0; break;\n";
    c += "        case 2: _a = _qv->avail + 4 + 2 * (_qv->last_avail % _qv->qnum); _l = 2; _wr = 0; break;\n";
    c += "        case 3: _a = _qv->desc + _qv->idx * 16; _l = 16; _wr = 0; break;\n";
    c += "        case 7: {\n";
    c += std::format("            _a = _qv->used + 4 + 8 * (_qv->used_idx % _qv->qnum); _l = 8; _wr = 1;\n", s.id);
    c += "            uint32_t _h = _qv->head;\n";
    c += std::format("            dcs_memcpy(_vio_mp_buf_{0}, &_h, 4);\n", s.id);
    c += "            uint32_t _n = _qv->written;\n";
    c += std::format("            dcs_memcpy(_vio_mp_buf_{0} + 4, &_n, 4);\n", s.id);
    c += "            break;\n";
    c += "        }\n";
    c += "        case 8: {\n";
    c += "            _a = _qv->used + 2; _l = 2; _wr = 1;\n";
    c += "            uint16_t _ni = (uint16_t)(_qv->used_idx + 1);\n";
    c += std::format("            dcs_memcpy(_vio_mp_buf_{0}, &_ni, 2);\n", s.id);
    c += "            break;\n";
    c += "        }\n";
    c += s.queues[q].start;
    c += "        default: break;\n";
    c += "        }\n";
    c += std::format("        if (_l) {{\n"
                     "            _vio_mp_active_{0} = 1; _vio_mp_addr_{0} = _a; _vio_mp_len_{0} = _l;\n"
                     "            _vio_mp_beat_{0} = 0; _vio_mp_write_{0} = _wr; _vio_mp_ackp_{0} = 0; _vio_mp_q_{0} = {1};\n"
                     "        }}\n"
                     "    }}\n",
                     s.id, q);
    return c;
}

} // namespace

std::string genSeq(const DeviceSpec &s) {
    const auto &n = s.in_nets;
    std::string c;
    c += std::format("// {}\n", dsc::cgen::commentText(s.node_name));
    c += std::format("static void {}() {{\n", s.func_name);
    // 空闲快速路径：绝大多数拍设备既没有 MMIO 事务、也没有在途的队列/主端口，
    // 这时只需要 stb/cyc 两根线和几个状态字就能判定。先做这个判定，剩下的
    // 7 根输入线、寄存器译码、队列状态机和 9 根输出线全部跳过。
    // 注意 _vio_prev_stb 必须照常更新，否则总线撤销那一拍看不到下降沿
    c += std::format("    uint8_t _stb = _w[{}][0] & 1;\n", n[I_STB]);
    c += std::format("    uint8_t _cyc = _w[{}][0] & 1;\n", n[I_CYC]);
    c += "    const uint8_t _active = _stb & _cyc;\n";
    c += std::format("    const uint8_t _prev_act = _vio_prev_stb_{};\n", s.id);
    c += std::format("    const uint8_t _rise = _active & (uint8_t) !_vio_prev_stb_{};\n", s.id);
    c += std::format("    _vio_prev_stb_{} = _active;\n", s.id);
    c += std::format(
            "    uint8_t _busy_opt = (uint8_t)(_active | _prev_act | _vio_mp_active_{0} | _vio_mp_gap_{0});\n", s.id);
    // 队列空闲时还要看宿主侧有没有新事件（网卡收包、input 事件），
    // 这些检查只碰宿主内存，和总线无关
    for (int q = 0; q < (int) s.nq; q++)
        c += genWake(s, q);
    // 唤醒之后、处理之前检查：既覆盖"进入本拍时已经很忙"，也覆盖"本拍刚被唤醒"
    for (int q = 0; q < (int) s.nq; q++)
        c += std::format("    _busy_opt |= (uint8_t)(_vio_q_{}[{}].phase != 0);\n", s.id, q);
    c += "    if (!_busy_opt) return;\n";

    c += std::format("    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[{}], 4);\n", n[I_ADR]);
    c += std::format("    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[{}], 4);\n", n[I_DAT]);
    c += std::format("    uint8_t _we = _w[{}][0] & 1;\n", n[I_WE]);
    // config 空间的寄存器里有字节粒度的字段（如 virtio-input 的 select/subsel），
    // 总线给出的是字节通道掩码，设备要按 _sel 把各通道拆开
    c += std::format("    uint8_t _sel = _w[{}][0] & 0x0F;\n", n[I_SEL]);
    c += std::format("    uint32_t _m_dat = 0; dcs_memcpy(&_m_dat, _w[{}], 4);\n", n[I_M_DAT]);
    c += std::format("    uint8_t _m_ack = _w[{}][0] & 1;\n", n[I_M_ACK]);
    c += "    uint32_t _dout = 0; uint8_t _ack = 0;\n";
    c += genMmio(s);
    c += genMaster(s);
    c += genDone(s);
    for (int q = 0; q < (int) s.nq; q++)
        c += genQueue(s, q);
    // 卡死检测：主端口一个 beat 正常只有几拍，队列阶段也不该长期停在非 0。
    // 各报一次就够定位，不会把日志刷爆
    c += std::format(
            "    if (_vio_mp_active_{0}) {{\n"
            "        if (_vio_wd_{0} != 0xFFFFFFFFu) _vio_wd_{0}++;\n"
            "        if (_vio_wd_{0} == 20000u)\n"
            "            dcs_virtio_trace({0}u, 0x100u | _vio_q_{0}[0].phase, _vio_mp_addr_{0}, _vio_mp_beat_{0}, "
            "_vio_mp_len_{0}, _vio_q_{0}[0].qready, _vio_mp_write_{0}, _vio_q_{0}[0].qnum);\n"
            "    }} else _vio_wd_{0} = 0;\n"
            "    if (!_vio_mp_active_{0} && _vio_q_{0}[0].phase != 0) {{\n"
            "        if (_vio_pd_{0} != 0xFFFFFFFFu) _vio_pd_{0}++;\n"
            "        if (_vio_pd_{0} == 20000u)\n"
            "            dcs_virtio_trace({0}u, 0x200u | _vio_q_{0}[0].phase, _vio_q_{0}[0].last_avail, "
            "_vio_q_{0}[0].used_idx, _vio_q_{0}[0].qnum, _vio_q_{0}[0].qready, _vio_q_{0}[0].desc, "
            "_vio_q_{0}[0].avail);\n"
            "    }} else _vio_pd_{0} = 0;\n",
            s.id);
    c += std::format("    const uint8_t _irq_out = _vio_int_{} != 0;\n", s.id);
    // 走到这里说明本拍一定有动作（总线事务撤销、队列推进或宿主事件），
    // 9 根输出线一律刷新：撤销那一拍必须把 ack/dat/stb 拉回去
    if (s.out_write) {
        c += s.out_write(O_DAT, "_dout", 32);
        c += s.out_write(O_ACK, "_ack", 1);
        c += s.out_write(O_IRQ, "_irq_out", 1);
        c += s.out_write(O_M_ADR, "_m_adr", 32);
        c += s.out_write(O_M_DAT, "_m_dout", 32);
        c += s.out_write(O_M_WE, "_m_we", 1);
        c += s.out_write(O_M_SEL, "_m_sel", 4);
        c += s.out_write(O_M_STB, "_m_stb", 1);
        c += s.out_write(O_M_CYC, "_m_cyc", 1);
    }
    c += "}\n";
    return c;
}

DeviceSpec makeBlkSpec(const BlkParams &p) {
    const std::string &id = p.id;
    DeviceSpec s;
    s.id = id;
    s.func_name = p.func_name;
    s.node_name = p.node_name;
    s.reset_symbol = p.reset_symbol;
    s.dev_id = 2;
    s.nq = 1;
    s.features = p.features;
    s.in_nets = p.in_nets;
    s.state_decl = p.state_decl;
    s.init_code = p.init_code;
    s.config_read = std::format(
            "            case 0x100: _dout = {}u; break;\n"
            "            case 0x104: _dout = {}u; break;\n",
            (uint32_t) (p.size_bytes / 512), (uint32_t) ((p.size_bytes / 512) >> 32));
    s.out_write = p.out_write;

    QueueSpec q;
    q.wake = "";
    // desc0 是 16 字节请求头；之后带 NEXT 的都是数据段，链尾没有 NEXT 的是状态字节
    q.desc = std::format(
            "                if (_qv->step == 0) {{\n"
            // 请求头不足 16 字节时不能照 16 字节去读 guest 内存（可能越过描述符甚至
            // 未映射区域），直接以 written=0 完成，别让设备卡在在途事务上
            "                    if (_qv->dlen < 16) _qv->phase = {3};\n"
            "                    else {{ _qv->step = 1; _qv->phase = {0}; }}\n"
            "                }} else if (_qv->dflags & 1) {{\n"
            // 零长度数据段没有可搬运的字节，直接跳到下一个描述符；
            // 否则 q.start 里 _l=0，genQueue 不启动主端口事务，请求永远卡在数据阶段
            "                    if (_qv->dlen == 0) {{ _qv->idx = _qv->dnext; _qv->phase = {4}; }}\n"
            "                    else _qv->phase = {1};\n"
            "                }} else {{\n"
            "                    _qv->phase = {2};\n"
            "                }}\n",
            Q_DEV, Q_DEV + 1, Q_DEV + 2, Q_USED, Q_DESC);
    q.start = std::format(
            "        case {0}: _a = _qv->daddr; _l = 16; _wr = 0; break;\n"
            "        case {3}: {{\n"
            "            uint32_t _c = _qv->dlen - _qv->off;\n"
            "            if (_c > 64) _c = 64;\n"
            "            _qv->chunk = _c;\n"
            "            _a = _qv->daddr + _qv->off; _l = _c;\n"
            "            if (_vio_type_{1} == 0) {{\n"
            // sector 字段是 64 位：偏移必须按 64 位算，否则 4GiB 以上的镜像会回绕到盘头。
            // guest 给的 sector 可以任意大，先查乘加溢出，溢出按 IOERR 拒绝
            "                if (_vio_sec_{1} > (~0ull - _qv->written) / 512ull) _vio_status_byte_{1} = 1;\n"
            "                else {{\n"
            "                uint64_t _io_off = _vio_sec_{1} * 512ull + _qv->written;\n"
            "{2}"
            "                }}\n"
            "                _wr = 1;\n"
            "            }} else _wr = 0;\n"
            "            break;\n"
            "        }}\n"
            "        case {4}: _a = _qv->daddr; _l = 1; _wr = 1; _vio_mp_buf_{1}[0] = _vio_status_byte_{1}; break;\n",
            Q_DEV, id, p.read_stmt, Q_DEV + 1, Q_DEV + 2);
    q.done = std::format(
            "            case {0}: {{ // 请求头读完，进入数据段\n"
            // 头描述符没有 NEXT 就没有状态描述符，按原流程会把状态字节写回请求头
            // 本身所在的 guest 内存。畸形链直接完成请求，不再碰 guest 缓冲
            "                if (!(_qv->dflags & 1)) {{ _qv->phase = {6}; break; }}\n"
            "                _vio_type_{1} = (uint32_t)_vio_mp_buf_{1}[0] | ((uint32_t)_vio_mp_buf_{1}[1] << 8) |\n"
            "                                ((uint32_t)_vio_mp_buf_{1}[2] << 16) | ((uint32_t)_vio_mp_buf_{1}[3] << 24);\n"
            // 规范里 virtio_blk_req.sector 是 64 位字段，低 32 位装不下大盘
            "                dcs_memcpy(&_vio_sec_{1}, _vio_mp_buf_{1} + 8, 8);\n"
            "                _qv->written = 0; _vio_status_byte_{1} = 0;\n"
            "                if (_qv->dflags & 1) {{ _qv->idx = _qv->dnext; _qv->phase = {4}; }}\n"
            "                else _qv->phase = {3};\n"
            "                break;\n"
            "            }}\n"
            "            case {3}: {{ // 数据段读完\n"
            "                uint32_t _c = _qv->chunk;\n"
            "                if (_vio_type_{1} == 0) _qv->written += _c;\n"
            "                else if (_vio_type_{1} == 1) {{\n"
            // 同 q.start：guest 的 sector 可以先查溢出，避免回绕后写到介质开头
            "                    if (_vio_sec_{1} > (~0ull - _qv->written) / 512ull) _vio_status_byte_{1} = 1;\n"
            "                    else {{\n"
            "                    uint64_t _io_off = _vio_sec_{1} * 512ull + _qv->written;\n"
            "{2}"
            "                    }}\n"
            "                    _qv->written += _c;\n"
            "                }} else if (_vio_type_{1} != 4) _vio_status_byte_{1} = 2;\n"
            "                _qv->off += _c;\n"
            "                if (_qv->off >= _qv->dlen) {{\n"
            "                    if (_qv->dflags & 1) {{ _qv->idx = _qv->dnext; _qv->phase = {4}; }}\n"
            "                    else _qv->phase = {5};\n"
            "                }}\n"
            "                break;\n"
            "            }}\n"
            "            case {5}: {{ // 状态字节写完\n"
            "                _qv->written = _vio_type_{1} == 0 ? _qv->written : 0;\n"
            "                _qv->phase = {6};\n"
            "                break;\n"
            "            }}\n",
            Q_DEV, id, p.write_stmt, Q_DEV + 1, Q_DESC, Q_DEV + 2, Q_USED);
    s.queues.push_back(q);
    return s;
}

} // namespace dsc::virtio_gen

// JIT 里检测到设备长时间不推进时调用。tag 高位区分是主端口卡住还是队列阶段卡住，
// 参数含义由生成代码决定，这里只负责原样打出来
extern "C" void dcs_virtio_trace(uint32_t id, uint32_t tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                 uint32_t e, uint32_t f) {
    // 0x1xx/0x2xx 是卡死看门狗，只在异常时触发，默认就打印；
    // 其余追踪点在每拍都会走到的路径上，默认关闭。DCS_VIRTIO_TRACE 支持
    // "1"/"all" 或逗号分隔的 tag 列表（如 "301,303,304,306"）
    struct Filter {
        bool all = false;
        uint32_t tags[16] = {};
        int n = 0;
        Filter() {
            const char *s = std::getenv("DCS_VIRTIO_TRACE");
            if (!s || !*s)
                return;
            if (s[0] == '1' && s[1] == 0) {
                all = true;
                return;
            }
            if (std::strcmp(s, "all") == 0) {
                all = true;
                return;
            }
            while (*s && n < 16) {
                char *end = nullptr;
                unsigned long v = std::strtoul(s, &end, 16);
                if (end == s)
                    break;
                tags[n++] = (uint32_t) v;
                s = end;
                while (*s == ',' || *s == ' ')
                    s++;
            }
        }
        bool on(uint32_t tag) const {
            if (tag < 0x300u)
                return true;
            if (all)
                return true;
            for (int i = 0; i < n; i++)
                if (tags[i] == tag)
                    return true;
            return false;
        }
    };
    static const Filter filter;
    if (!filter.on(tag))
        return;
    std::fprintf(stderr, "[dcs] virtio#%u tag=0x%X a=%u b=%u c=%u d=%u e=%u f=%u\n", id, tag, a, b, c, d, e, f);
    std::fflush(stderr);
}