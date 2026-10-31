// VirtioMmio 的 input 队列状态机
//
// 真实 virtio-input：驱动提交一个可写 buffer，设备把 evdev 事件（每个 8 字节：
// type u16 / code u16 / value i32）写进去，used.len 是写入字节数。
// 事件由宿主通过 pushInputEvent() 放进 State 的事件环，JIT 每拍直接读 head/tail，
// 没有跨模块回调。
#include "dcs/components/platform/VirtioInternal.h"
#include "dcs/components/platform/VirtioMmio.h"

#include <cstddef>
#include <format>

namespace dsc {

using namespace virtio;

namespace {
constexpr int Q_EVENTS = 9; // input 特有阶段：把事件搬进 guest buffer
}

std::string VirtioMmio::genInputDone() const {
    return std::format(
            "    if (_mp_done) {{\n"
            "        switch (_vio_phase_{0}) {{\n"
            "        case {1}: {{ // avail.idx\n"
            "            _vio_avail_cache_{0} = (uint32_t)_vio_buf_{0}[0] | ((uint32_t)_vio_buf_{0}[1] << 8);\n"
            "            _vio_phase_{0} = (_vio_avail_cache_{0} == _vio_last_avail_{0}) ? 0 : {2};\n"
            "            break;\n"
            "        }}\n"
            "        case {2}: {{ // avail.ring\n"
            "            _vio_head_{0} = (uint32_t)_vio_buf_{0}[0] | ((uint32_t)_vio_buf_{0}[1] << 8);\n"
            "            _vio_idx_{0} = _vio_head_{0}; _vio_written_{0} = 0;\n"
            "            _vio_phase_{0} = {3};\n"
            "            break;\n"
            "        }}\n"
            "        case {3}: {{ // descriptor\n"
            "            uint64_t _a = 0; dcs_memcpy(&_a, _vio_buf_{0}, 8);\n"
            "            _vio_daddr_{0} = (uint32_t)_a;\n"
            "            _vio_dlen_{0} = (uint32_t)_vio_buf_{0}[8] | ((uint32_t)_vio_buf_{0}[9] << 8) |\n"
            "                           ((uint32_t)_vio_buf_{0}[10] << 16) | ((uint32_t)_vio_buf_{0}[11] << 24);\n"
            "            _vio_dflags_{0} = (uint16_t)((uint32_t)_vio_buf_{0}[12] | ((uint32_t)_vio_buf_{0}[13] << 8));\n"
            "            _vio_dnext_{0} = (uint32_t)_vio_buf_{0}[14] | ((uint32_t)_vio_buf_{0}[15] << 8);\n"
            "            _vio_off_{0} = 0;\n"
            // 装不下一个事件（< 8 字节）的 buffer 直接以 len=0 完成，不能让队列卡住
            "            _vio_phase_{0} = _vio_dlen_{0} < 8 ? {5} : {4};\n"
            "            break;\n"
            "        }}\n"
            "        case {4}: {{ // 事件块写完\n"
            "            *(uint32_t *)((char *)_vio_state_{0} + {7}) = _vio_evt_new_tail_{0};\n"
            "            _vio_written_{0} += _vio_step_{0};\n"
            // used.len 是本次实际写入的字节数，不要求填满整个 buffer：
            // 驱动给 64 字节而设备只有 2 个事件时，必须立刻以 16 字节完成
            "            _vio_phase_{0} = {5};\n"
            "            break;\n"
            "        }}\n"
            "        case {5}: _vio_phase_{0} = {6}; break;\n"
            "        case {6}: {{ _vio_used_idx_{0}++; _vio_last_avail_{0}++; _vio_int_{0} |= 1u; _vio_phase_{0} = {1}; break; }}\n"
            "        default: _vio_phase_{0} = 0; break;\n"
            "        }}\n"
            "    }}\n",
            _id, Q_AVAIL_IDX, Q_AVAIL_RING, Q_DESC, Q_EVENTS, Q_USED, Q_USED_IDX,
            (unsigned long long) offsetof(State, evt_tail));
}

std::string VirtioMmio::genInputStart() const {
    const unsigned long long head_off = offsetof(State, evt_head);
    const unsigned long long tail_off = offsetof(State, evt_tail);
    const unsigned long long evt_off = offsetof(State, events);
    const unsigned ring = State::EVT_RING;

    // 生成期展开 8 个事件槽：常量下标，没有循环
    std::string copy_events;
    for (int k = 0; k < 8; k++) {
        copy_events += std::format(
                "            if ({0}u < _n) dcs_memcpy(_vio_buf_{1} + {2}, "
                "(char *)_vio_state_{1} + {3} + (({0}u + _vio_evt_tail_{1}) % {4}u) * 8, 8);\n",
                k, _id, k * 8, evt_off, ring);
    }

    return std::format(
            "    // 宿主 push 了新事件才唤醒队列；空闲时只读两个整数\n"
            "    if (_vio_phase_{0} == 0 && _vio_qready_{0}) {{\n"
            "        uint32_t _h = *(uint32_t *)((char *)_vio_state_{0} + {7});\n"
            "        uint32_t _t = *(uint32_t *)((char *)_vio_state_{0} + {8});\n"
            "        if (_h != _t) _vio_phase_{0} = {1};\n"
            "    }}\n"
            "    if (!_vio_mp_active_{0} && !_mp_gap_now && _vio_phase_{0} != 0) {{\n"
            "        {{\n"
            "        uint32_t _a = 0, _l = 0; uint8_t _wr = 0;\n"
            "        switch (_vio_phase_{0}) {{\n"
            "        case {1}: _a = _vio_avail_{0} + 2; _l = 2; _wr = 0; break;\n"
            "        case {2}: _a = _vio_avail_{0} + 4 + 2 * (_vio_last_avail_{0} % _vio_qnum_{0}); _l = 2; _wr = 0; break;\n"
            "        case {3}: _a = _vio_desc_{0} + _vio_idx_{0} * 16; _l = 16; _wr = 0; break;\n"
            "        case {4}: {{\n"
            "            uint32_t _h = *(uint32_t *)((char *)_vio_state_{0} + {7});\n"
            "            uint32_t _t = *(uint32_t *)((char *)_vio_state_{0} + {8});\n"
            "            uint32_t _n = _h - _t;\n"
            "            uint32_t _cap = _vio_dlen_{0} / 8;\n"
            "            if (_n > _cap) _n = _cap;\n"
            "            if (_n > 8) _n = 8;\n"
            "            if (_n == 0) break; // 暂时没有事件：保持阶段，等宿主 push\n"
            "            _vio_evt_tail_{0} = _t;\n"
            "            _vio_evt_n_{0} = _n;\n"
            "{9}"
            "            _vio_step_{0} = _n * 8; _vio_evt_new_tail_{0} = _t + _n;\n"
            "            _a = _vio_daddr_{0}; _l = _vio_step_{0}; _wr = 1;\n"
            "            break;\n"
            "        }}\n"
            "        case {5}: {{\n"
            "            _a = _vio_used_{0} + 4 + 8 * (_vio_used_idx_{0} % _vio_qnum_{0}); _l = 8; _wr = 1;\n"
            "            uint32_t _head = _vio_head_{0}; dcs_memcpy(_vio_buf_{0}, &_head, 4);\n"
            "            uint32_t _n = _vio_written_{0}; dcs_memcpy(_vio_buf_{0} + 4, &_n, 4);\n"
            "            break;\n"
            "        }}\n"
            "        case {6}: {{\n"
            "            _a = _vio_used_{0} + 2; _l = 2; _wr = 1;\n"
            "            uint16_t _ni = (uint16_t)(_vio_used_idx_{0} + 1); dcs_memcpy(_vio_buf_{0}, &_ni, 2);\n"
            "            break;\n"
            "        }}\n"
            "        default: break;\n"
            "        }}\n"
            "        if (_l) {{\n"
            "            _vio_mp_active_{0} = 1; _vio_mp_addr_{0} = _a; _vio_mp_len_{0} = _l;\n"
            "            _vio_mp_beat_{0} = 0; _vio_mp_write_{0} = _wr; _vio_mp_ackp_{0} = 0;\n"
            "        }}\n"
            "        }}\n"
            "    }}\n",
            _id, Q_AVAIL_IDX, Q_AVAIL_RING, Q_DESC, Q_EVENTS, Q_USED, Q_USED_IDX, head_off, tail_off, copy_events);
}

} // namespace dsc
