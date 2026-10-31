// VirtioMmio 的 rng 队列状态机（从 VirtioMmio.cpp 拆出）
#include "dcs/components/platform/VirtioInternal.h"
#include "dcs/components/platform/VirtioMmio.h"

#include <format>

namespace dsc {

using namespace virtio;

std::string VirtioMmio::genRngDone() const {
        return std::format(
                "    if (_mp_done) {{\n"
                "        switch (_vio_phase_{0}) {{\n"
                "        case {1}: {{\n"
                "            _vio_avail_cache_{0} = (uint32_t)_vio_buf_{0}[0] | ((uint32_t)_vio_buf_{0}[1] << 8);\n"
                "            _vio_phase_{0} = (_vio_avail_cache_{0} == _vio_last_avail_{0}) ? 0 : {2};\n"
                "            break;\n"
                "        }}\n"
                "        case {2}: {{\n"
                "            _vio_head_{0} = (uint32_t)_vio_buf_{0}[0] | ((uint32_t)_vio_buf_{0}[1] << 8);\n"
                "            _vio_idx_{0} = _vio_head_{0}; _vio_written_{0} = 0;\n"
                "            _vio_phase_{0} = {3};\n"
                "            break;\n"
                "        }}\n"
                "        case {3}: {{\n"
                "            uint64_t _a = 0; dcs_memcpy(&_a, _vio_buf_{0}, 8);\n"
                "            _vio_daddr_{0} = (uint32_t)_a;\n"
                "            _vio_dlen_{0} = (uint32_t)_vio_buf_{0}[8] | ((uint32_t)_vio_buf_{0}[9] << 8) |\n"
                "                           ((uint32_t)_vio_buf_{0}[10] << 16) | ((uint32_t)_vio_buf_{0}[11] << 24);\n"
                "            _vio_dflags_{0} = (uint16_t)((uint32_t)_vio_buf_{0}[12] | ((uint32_t)_vio_buf_{0}[13] << 8));\n"
                "            _vio_dnext_{0} = (uint32_t)_vio_buf_{0}[14] | ((uint32_t)_vio_buf_{0}[15] << 8);\n"
                "            _vio_off_{0} = 0;\n"
                "            _vio_phase_{0} = {4};\n"
                "            break;\n"
                "        }}\n"
                "        case {4}: {{\n"
                "            _vio_off_{0} += _vio_step_{0};\n"
                "            _vio_written_{0} += _vio_step_{0};\n"
                "            if (_vio_off_{0} >= _vio_dlen_{0}) {{\n"
                "                if (_vio_dflags_{0} & 1) {{ _vio_idx_{0} = _vio_dnext_{0}; _vio_phase_{0} = {3}; }}\n"
                "                else _vio_phase_{0} = {5};\n"
                "            }}\n"
                "            break;\n"
                "        }}\n"
                "        case {5}: _vio_phase_{0} = {6}; break;\n"
                "        case {6}: {{ _vio_used_idx_{0}++; _vio_last_avail_{0}++; _vio_int_{0} |= 1u; _vio_phase_{0} = {1}; break; }}\n"
                "        default: _vio_phase_{0} = 0; break;\n"
                "        }}\n"
                "    }}\n",
                _id, Q_AVAIL_IDX, Q_AVAIL_RING, Q_DESC, Q_DATA, Q_USED, Q_USED_IDX);
}

std::string VirtioMmio::genRngStart() const {
    // 随机数在 JIT 里用 xorshift64 生成，生成期展开成 8 组直线代码
    std::string rng_fill;
    for (int k = 0; k < 8; k++) {
        rng_fill += "            _r ^= _r << 13; _r ^= _r >> 7; _r ^= _r << 17;\n";
        rng_fill += std::format("            dcs_memcpy(_vio_buf_{} + {}, &_r, 8);\n", _id, k * 8);
    }
    return std::format(
                "    if (!_vio_mp_active_{0} && !_mp_gap_now && _vio_phase_{0} != 0) {{\n"
                "        {{\n"
                "        uint32_t _a = 0, _l = 0; uint8_t _wr = 0;\n"
                "        switch (_vio_phase_{0}) {{\n"
                "        case {1}: _a = _vio_avail_{0} + 2; _l = 2; _wr = 0; break;\n"
                "        case {2}: _a = _vio_avail_{0} + 4 + 2 * (_vio_last_avail_{0} % _vio_qnum_{0}); _l = 2; _wr = 0; break;\n"
                "        case {3}: _a = _vio_desc_{0} + _vio_idx_{0} * 16; _l = 16; _wr = 0; break;\n"
                "        case {4}: {{\n"
                "            uint32_t _chunk = _vio_dlen_{0} - _vio_off_{0};\n"
                "            if (_chunk > 64) _chunk = 64;\n"
                "            _vio_step_{0} = _chunk;\n"
                "            uint64_t _r = _vio_rng_{0};\n"
                "{7}"
                "            _vio_rng_{0} = _r;\n"
                "            _a = _vio_daddr_{0} + _vio_off_{0}; _l = _chunk; _wr = 1;\n"
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
                _id, Q_AVAIL_IDX, Q_AVAIL_RING, Q_DESC, Q_DATA, Q_USED, Q_USED_IDX, rng_fill);
}

} // namespace dsc
