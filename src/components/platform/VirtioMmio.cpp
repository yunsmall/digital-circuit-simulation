// VirtioMmio — virtio-mmio（modern v2）设备
//
// 从端口接 CPU 读写寄存器；主端口像 DMA 一样发起总线事务，访问 guest 内存里的
// descriptor/avail/used 环和数据缓冲。
//
// 寄存器、队列状态机、总线事务全部生成进 JIT：每拍推进一步，没有任何跨模块
// 回调。唯一的 C++ 回调是 ioAccess——镜像在宿主文件或 ROM 里，只能由 C++ 读写。
#include "dcs/components/platform/VirtioMmio.h"

#include "dcs/Circuit.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/memory/ROM.h"

#include <cstdio>
#include <cstring>
#include <format>

namespace dsc {

namespace {
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_M_DAT, I_M_ACK };
enum OutIdx { O_DAT = 0, O_ACK, O_IRQ, O_M_ADR, O_M_DAT, O_M_WE, O_M_SEL, O_M_STB, O_M_CYC };

// modern virtio-mmio 寄存器偏移（version 2）
constexpr uint32_t R_MAGIC = 0x000;
constexpr uint32_t R_VERSION = 0x004;
constexpr uint32_t R_DEVICE_ID = 0x008;
constexpr uint32_t R_VENDOR_ID = 0x00C;
constexpr uint32_t R_DEVICE_FEATURES = 0x010;
constexpr uint32_t R_DEVICE_FEATURES_SEL = 0x014;
constexpr uint32_t R_DRIVER_FEATURES = 0x020;
constexpr uint32_t R_DRIVER_FEATURES_SEL = 0x024;
constexpr uint32_t R_QUEUE_SEL = 0x030;
constexpr uint32_t R_QUEUE_NUM_MAX = 0x034;
constexpr uint32_t R_QUEUE_NUM = 0x038;
constexpr uint32_t R_QUEUE_READY = 0x044;
constexpr uint32_t R_QUEUE_NOTIFY = 0x050;
constexpr uint32_t R_INTERRUPT_STATUS = 0x060;
constexpr uint32_t R_INTERRUPT_ACK = 0x064;
constexpr uint32_t R_STATUS = 0x070;
constexpr uint32_t R_DESC_LOW = 0x080, R_DESC_HIGH = 0x084;
constexpr uint32_t R_AVAIL_LOW = 0x090, R_AVAIL_HIGH = 0x094;
constexpr uint32_t R_USED_LOW = 0x0A0, R_USED_HIGH = 0x0A4;
constexpr uint32_t R_CONFIG_GEN = 0x0FC;
constexpr uint32_t R_CONFIG = 0x100;

constexpr uint32_t VIRTIO_MMIO_MAGIC = 0x74726976; // "virt"
constexpr uint32_t VIRTIO_VENDOR = 0x444353;       // "DCS"
constexpr uint32_t QUEUE_MAX = 8;

// 队列状态机阶段（生成代码里用字面量）
constexpr int Q_IDLE = 0, Q_AVAIL_IDX = 1, Q_AVAIL_RING = 2, Q_DESC = 3, Q_HDR = 4, Q_DATA = 5,
              Q_STATUS = 6, Q_USED = 7, Q_USED_IDX = 8;

// blk 请求类型与状态字节
constexpr uint32_t BLK_T_IN = 0, BLK_T_OUT = 1, BLK_T_FLUSH = 4;
constexpr uint8_t BLK_S_OK = 0, BLK_S_IOERR = 1, BLK_S_UNSUPP = 2, BLK_S_RO = 3;
} // namespace

struct VirtioMmio::State {
    std::FILE *fp = nullptr;
    const uint8_t *image = nullptr;
    uint64_t image_size = 0;
    bool read_only = false;
};

VirtioMmio::VirtioMmio(const std::string &name, Kind kind, const std::string &path) :
    SequentialComponent(name, "virtio_mmio"), _state(std::make_unique<State>()), _kind(kind), _path(path) {
    setParam("device", kind == Kind::Blk ? "blk" : "console");
    setParam("path", path);
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

VirtioMmio::~VirtioMmio() {
    if (_state->fp)
        std::fclose(_state->fp);
}

bool VirtioMmio::prepare(std::string &error) {
    if (_kind == Kind::Blk && !_path.empty()) {
        _state->fp = std::fopen(_path.c_str(), "r+b");
        if (!_state->fp) {
            _state->fp = std::fopen(_path.c_str(), "rb");
            _state->read_only = true;
        }
        if (!_state->fp) {
            error = std::format("virtio blk 无法打开镜像 '{}'", _path);
            return false;
        }
        std::fseek(_state->fp, 0, SEEK_END);
        _state->image_size = (uint64_t) std::ftell(_state->fp);
        std::fseek(_state->fp, 0, SEEK_SET);
    }
    _prepared = true;
    return true;
}

void VirtioMmio::bindCircuit(Circuit *circuit) {
    if (_state->fp || _kind != Kind::Blk)
        return;
    for (auto &c: circuit->components()) {
        if (auto *r = dynamic_cast<ROM *>(c.get())) {
            _state->image = r->readOnlyMemPtr();
            _state->image_size = r->memSize();
            _state->read_only = true;
            break;
        }
    }
}

std::vector<Component::JitSymbol> VirtioMmio::extraJitSymbols() const {    return {
            {"dcs_virtio_io", reinterpret_cast<void *>(&VirtioMmio::ioAccess)},
    };
}

std::string VirtioMmio::genStateDecl() const {
    const uint32_t dev_id = _kind == Kind::Blk ? 2 : 3;
    const uint32_t cap = (uint32_t) (_state->image_size / 512);
    return std::format(
            "#define _VIO_{0}_MAGIC 0x{1:X}u\n"
            "#define _VIO_{0}_DEVID {2}u\n"
            "#define _VIO_{0}_CAPLO {3}u\n"
            "#define _VIO_{0}_CAPHI {4}u\n"
            "#define _VIO_{0}_RO {5}u\n"
            // 寄存器
            "static uint32_t _vio_status_{0}, _vio_int_{0}, _vio_qsel_{0}, _vio_qnum_{0}, _vio_qready_{0};\n"
            "static uint32_t _vio_dsel_{0}, _vio_rsel_{0};\n"
            "static uint32_t _vio_desc_{0}, _vio_avail_{0}, _vio_used_{0};\n"
            "static uint32_t _vio_last_avail_{0}, _vio_used_idx_{0};\n"
            "static uint8_t _vio_prev_stb_{0};\n"
            // 队列状态机
            "static uint8_t _vio_phase_{0}, _vio_have_hdr_{0};\n"
            "static uint32_t _vio_avail_cache_{0}, _vio_head_{0}, _vio_idx_{0};\n"
            "static uint32_t _vio_daddr_{0}, _vio_dlen_{0}, _vio_dnext_{0};\n"
            "static uint16_t _vio_dflags_{0};\n"
            "static uint32_t _vio_type_{0}, _vio_sec_lo_{0}, _vio_sec_hi_{0};\n"
            "static uint32_t _vio_off_{0}, _vio_written_{0};\n"
            "static uint8_t _vio_st_{0};\n"
            // 主端口事务
            "static uint8_t _vio_mp_active_{0}, _vio_mp_write_{0}, _vio_mp_ackp_{0}, _vio_mp_done_{0};\n"
            "static uint8_t _vio_mp_gap_{0};\n"
            "static uint32_t _vio_mp_addr_{0}, _vio_mp_len_{0}, _vio_mp_beat_{0};\n"
            "static uint8_t _vio_buf_{0}[64];\n"
            "static void *_vio_state_{0};\n"
            "extern uint32_t dcs_virtio_io(void *, uint32_t, uint32_t, uint8_t *, uint32_t, uint32_t);\n",
            _id, VIRTIO_MMIO_MAGIC, dev_id, cap, 0u, _state->read_only ? 1u : 0u);
}

std::string VirtioMmio::genInitCode() const {
    return std::format(
            "    _vio_status_{0} = 0; _vio_int_{0} = 0; _vio_qsel_{0} = 0; _vio_qnum_{0} = 0;\n"
            "    _vio_qready_{0} = 0; _vio_dsel_{0} = 0; _vio_rsel_{0} = 0;\n"
            "    _vio_desc_{0} = 0; _vio_avail_{0} = 0; _vio_used_{0} = 0;\n"
            "    _vio_last_avail_{0} = 0; _vio_used_idx_{0} = 0; _vio_prev_stb_{0} = 0;\n"
            "    _vio_phase_{0} = 0; _vio_have_hdr_{0} = 0; _vio_mp_active_{0} = 0; _vio_mp_done_{0} = 0;\n"
            "    _vio_mp_gap_{0} = 0;\n"
            "    _vio_state_{0} = (void *)0x{1:X}ULL;\n",
            _id, reinterpret_cast<uintptr_t>(_state.get()));
}

std::string VirtioMmio::genFuncDef_seq() const {
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// VirtioMmio {}\n", name());
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(I_ADR) + "], 4);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_DAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _stb = _w[" + nid(I_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(I_CYC) + "][0] & 1;\n";
    c += "    uint32_t _m_dat = 0; dcs_memcpy(&_m_dat, _w[" + nid(I_M_DAT) + "], 4);\n";
    c += "    uint8_t _m_ack = _w[" + nid(I_M_ACK) + "][0] & 1;\n";
    c += "    uint32_t _dout = 0; uint8_t _ack = 0;\n";
    c += std::format(
            "    // ---- 从端口：MMIO 寄存器（读写的都是生成代码里的 static 状态）----\n"
            "    const uint8_t _active = _stb & _cyc;\n"
            "    const uint8_t _rise = _active & (uint8_t) !_vio_prev_stb_{0};\n"
            "    _vio_prev_stb_{0} = _active;\n"
            "    if (_active) {{\n"
            "        const uint32_t _off = _adr & 0xFFFu;\n"
            "        if (!_we) {{\n"
            "            switch (_off) {{\n"
            "            case 0x000: _dout = _VIO_{0}_MAGIC; break;\n"
            "            case 0x004: _dout = 2; break;\n"
            "            case 0x008: _dout = _VIO_{0}_DEVID; break;\n"
            "            case 0x00C: _dout = 0x444353u; break;\n"
            "            case 0x010:\n"
            "                if (_vio_dsel_{0} == 0) _dout = {1}u;\n"
            "                else if (_vio_dsel_{0} == 1) _dout = 1u;\n"
            "                break;\n"
            "            case 0x034: _dout = {2}u; break;\n"
            "            case 0x044: _dout = _vio_qready_{0}; break;\n"
            "            case 0x060: _dout = _vio_int_{0}; break;\n"
            "            case 0x070: _dout = _vio_status_{0}; break;\n"
            "            case 0x0FC: _dout = 0; break;\n"
            "            case 0x100: _dout = _VIO_{0}_CAPLO; break;\n"
            "            case 0x104: _dout = _VIO_{0}_CAPHI; break;\n"
            "            default: _dout = 0; break;\n"
            "            }}\n"
            "        }} else if (_rise) {{\n"
            "            switch (_off) {{\n"
            "            case 0x014: _vio_dsel_{0} = _wdat & 1; break;\n"
            "            case 0x024: _vio_rsel_{0} = _wdat & 1; break;\n"
            "            case 0x030: _vio_qsel_{0} = _wdat; break;\n"
            "            case 0x038: _vio_qnum_{0} = _wdat > {2}u ? {2}u : _wdat; break;\n"
            "            case 0x044:\n"
            "                _vio_qready_{0} = _wdat & 1;\n"
            "                _vio_last_avail_{0} = 0; _vio_used_idx_{0} = 0; _vio_phase_{0} = 0;\n"
            "                break;\n"
            "            case 0x050: _vio_phase_{0} = 1; break;\n" // notify：从读 avail.idx 开始
            "            case 0x064: _vio_int_{0} &= ~_wdat; break;\n"
            "            case 0x070:\n"
            "                _vio_status_{0} = _wdat;\n"
            "                if (_wdat == 0) {{\n"
            "                    _vio_int_{0} = 0; _vio_qready_{0} = 0; _vio_qnum_{0} = 0;\n"
            "                    _vio_last_avail_{0} = 0; _vio_used_idx_{0} = 0; _vio_phase_{0} = 0;\n"
            "                }}\n"
            "                break;\n"
            "            case 0x080: _vio_desc_{0} = (_vio_desc_{0} & 0xFFFF0000u) | (_wdat & 0xFFFFu); break;\n"
            "            case 0x084: _vio_desc_{0} = (_vio_desc_{0} & 0xFFFFu) | (_wdat << 16); break;\n"
            "            case 0x090: _vio_avail_{0} = (_vio_avail_{0} & 0xFFFF0000u) | (_wdat & 0xFFFFu); break;\n"
            "            case 0x094: _vio_avail_{0} = (_vio_avail_{0} & 0xFFFFu) | (_wdat << 16); break;\n"
            "            case 0x0A0: _vio_used_{0} = (_vio_used_{0} & 0xFFFF0000u) | (_wdat & 0xFFFFu); break;\n"
            "            case 0x0A4: _vio_used_{0} = (_vio_used_{0} & 0xFFFFu) | (_wdat << 16); break;\n"
            "            case 0x020:\n"
            "                break;\n" // driver features：不校验，全部接受
            "            default: break;\n"
            "            }}\n"
            "        }}\n"
            "        _ack = 1;\n"
            "    }}\n",
            _id, _state->read_only ? 32u : 0u, QUEUE_MAX);
    // 主端口事务
    c += std::format(
            "    // ---- 主端口事务：每拍最多 4 字节，按字节使能处理非对齐访问 ----\n"
            "    // Wishbone 从设备在 stb 上升沿处理事务，所以每搬完一个 beat 要撤销\n"
            "    // 一拍 stb（_vio_mp_gap），否则从设备不会产生新的 ack\n"
            "    uint32_t _m_adr = 0, _m_dout = 0;\n"
            "    uint8_t _m_we = 0, _m_stb = 0, _m_cyc = 0, _mp_done = 0;\n"
            "    uint8_t _m_sel = 0, _mp_gap_now = 0;\n"
            "    if (_vio_mp_active_{0}) {{\n"
            "        if (_vio_mp_gap_{0}) {{\n"
            "            _vio_mp_gap_{0} = 0; _vio_mp_ackp_{0} = 0; _mp_gap_now = 1;\n"
            "        }} else {{\n"
            "            uint32_t _step = _vio_mp_len_{0} - _vio_mp_beat_{0};\n"
            "            if (_step > 4) _step = 4;\n"
            "            const uint32_t _a = _vio_mp_addr_{0} + _vio_mp_beat_{0};\n"
            "            const uint32_t _shift = (_a & 3) * 8;\n"
            "            _m_adr = _a; _m_stb = 1; _m_cyc = 1; _m_we = _vio_mp_write_{0};\n"
            "            _m_sel = (uint8_t)(((1u << _step) - 1u) << (_a & 3));\n"
            "            if (_vio_mp_write_{0}) {{\n"
            "                uint32_t _v = 0; dcs_memcpy(&_v, _vio_buf_{0} + _vio_mp_beat_{0}, 4);\n"
            "                _m_dout = _v << _shift;\n"
            "            }}\n"
            "            if (_m_ack && !_vio_mp_ackp_{0}) {{\n"
            "                if (!_vio_mp_write_{0}) {{\n"
            "                    uint32_t _v = _m_dat >> _shift;\n"
            "                    dcs_memcpy(_vio_buf_{0} + _vio_mp_beat_{0}, &_v, _step);\n"
            "                }}\n"
            "                _vio_mp_beat_{0} += _step;\n"
            "                if (_vio_mp_beat_{0} >= _vio_mp_len_{0}) {{ _vio_mp_active_{0} = 0; _mp_done = 1; }}\n"
            "                _vio_mp_gap_{0} = 1; // 每个 beat 之后都要撤销一拍 stb\n"
            "            }}\n"
            "            _vio_mp_ackp_{0} = _m_ack;\n"
            "        }}\n"
            "    }} else if (_vio_mp_gap_{0}) {{\n"
            "        _vio_mp_gap_{0} = 0; _vio_mp_ackp_{0} = 0; _mp_gap_now = 1;\n"
            "    }}\n",
            _id);
    // 队列状态机：完成处理后推进阶段，空闲时发起下一个事务
    c += std::format(
            "    // ---- virtqueue 状态机（完成处理）----\n"
            "    if (_mp_done) {{\n"
            "        switch (_vio_phase_{0}) {{\n"
            "        case {1}: {{ // avail.idx\n"
            "            _vio_avail_cache_{0} = (uint32_t)_vio_buf_{0}[0] | ((uint32_t)_vio_buf_{0}[1] << 8);\n"
            "            if (_vio_avail_cache_{0} == _vio_last_avail_{0}) _vio_phase_{0} = 0;\n"
            "            else _vio_phase_{0} = {2};\n"
            "            break;\n"
            "        }}\n"
            "        case {2}: {{ // avail.ring[i]\n"
            "            _vio_head_{0} = (uint32_t)_vio_buf_{0}[0] | ((uint32_t)_vio_buf_{0}[1] << 8);\n"
            "            _vio_idx_{0} = _vio_head_{0}; _vio_have_hdr_{0} = 0;\n"
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
            "            _vio_phase_{0} = _vio_have_hdr_{0} ? ((_vio_dflags_{0} & 1) ? {5} : {7}) : {4};\n"
            "            if (!_vio_have_hdr_{0}) _vio_have_hdr_{0} = 1;\n"
            "            else _vio_off_{0} = 0;\n"
            "            break;\n"
            "        }}\n"
            "        case {4}: {{ // 请求头（16 字节）\n"
            "            _vio_type_{0} = (uint32_t)_vio_buf_{0}[0] | ((uint32_t)_vio_buf_{0}[1] << 8) |\n"
            "                           ((uint32_t)_vio_buf_{0}[2] << 16) | ((uint32_t)_vio_buf_{0}[3] << 24);\n"
            "            _vio_sec_lo_{0} = (uint32_t)_vio_buf_{0}[8] | ((uint32_t)_vio_buf_{0}[9] << 8) |\n"
            "                             ((uint32_t)_vio_buf_{0}[10] << 16) | ((uint32_t)_vio_buf_{0}[11] << 24);\n"
            "            _vio_sec_hi_{0} = 0;\n"
            "            _vio_written_{0} = 0; _vio_st_{0} = {6};\n"
            "            if (_vio_dflags_{0} & 1) {{ _vio_idx_{0} = _vio_dnext_{0}; _vio_phase_{0} = {3}; }}\n"
            "            else _vio_phase_{0} = {7};\n"
            "            break;\n"
            "        }}\n"
            "        case {5}: {{ // 数据段\n"
            "            uint32_t _chunk = _vio_dlen_{0} - _vio_off_{0};\n"
            "            if (_chunk > 64) _chunk = 64;\n"
            "            if (_vio_type_{0} == {8}) _vio_written_{0} += _chunk;\n"
            "            else if (_vio_type_{0} == {9}) {{\n"
            "                uint32_t _ok = dcs_virtio_io(_vio_state_{0}, _vio_sec_lo_{0} * 512u + _vio_written_{0}, 0,\n"
            "                                             _vio_buf_{0}, _chunk, 1);\n"
            "                if (!_ok) _vio_st_{0} = _VIO_{0}_RO ? 3u : {10};\n"
            "                _vio_written_{0} += _chunk;\n"
            "            }} else if (_vio_type_{0} != {11}) _vio_st_{0} = {12};\n"
            "            _vio_off_{0} += _chunk;\n"
            "            if (_vio_off_{0} >= _vio_dlen_{0}) {{\n"
            "                if (_vio_dflags_{0} & 1) {{ _vio_idx_{0} = _vio_dnext_{0}; _vio_have_hdr_{0} = 1; _vio_phase_{0} = {3}; }}\n"
            "                else _vio_phase_{0} = {7};\n"
            "            }}\n"
            "            break;\n"
            "        }}\n"
            "        case {7}: _vio_phase_{0} = {13}; break; // 状态字节写完\n"
            "        case {13}: _vio_phase_{0} = {14}; break; // used ring 写完\n"
            "        case {14}: {{ // used.idx 写完\n"
            "            _vio_used_idx_{0}++; _vio_last_avail_{0}++;\n"
            "            _vio_int_{0} |= 1u; _vio_phase_{0} = 0;\n"
            "            break;\n"
            "        }}\n"
            "        default: _vio_phase_{0} = 0; break;\n"
            "        }}\n"
            "    }}\n",
            _id, Q_AVAIL_IDX, Q_AVAIL_RING, Q_DESC, Q_HDR, Q_DATA, BLK_S_OK, Q_STATUS, BLK_T_IN, BLK_T_OUT,
            (uint32_t) BLK_S_IOERR, BLK_T_FLUSH, (uint32_t) BLK_S_UNSUPP, Q_USED, Q_USED_IDX);
    // 发起下一个事务
    c += std::format(
            "    // ---- virtqueue 状态机（发起事务）----\n"
            "    if (!_vio_mp_active_{0} && !_mp_gap_now && _vio_phase_{0} != 0) {{\n"
            "        {{\n"
            "        uint32_t _a = 0, _l = 0; uint8_t _wr = 0;\n"
            "        switch (_vio_phase_{0}) {{\n"
            "        case {1}: _a = _vio_avail_{0} + 2; _l = 2; _wr = 0; break;\n"
            "        case {2}: _a = _vio_avail_{0} + 4 + 2 * (_vio_last_avail_{0} % _vio_qnum_{0}); _l = 2; _wr = 0; break;\n"
            "        case {3}: _a = _vio_desc_{0} + _vio_idx_{0} * 16; _l = 16; _wr = 0; break;\n"
            "        case {4}: _a = _vio_daddr_{0}; _l = 16; _wr = 0; break;\n"
            "        case {5}: {{\n"
            "            uint32_t _chunk = _vio_dlen_{0} - _vio_off_{0};\n"
            "            if (_chunk > 64) _chunk = 64;\n"
            "            _a = _vio_daddr_{0} + _vio_off_{0}; _l = _chunk;\n"
            "            if (_vio_type_{0} == {6}) {{ // 设备读镜像 -> 写 guest\n"
            "                uint32_t _ok = dcs_virtio_io(_vio_state_{0}, _vio_sec_lo_{0} * 512u + _vio_written_{0}, 0,\n"
            "                                             _vio_buf_{0}, _chunk, 0);\n"
            "                if (!_ok) _vio_st_{0} = {7};\n"
            "                _wr = 1;\n"
            "            }} else _wr = 0; // 写请求：读 guest 后再写镜像\n"
            "            break;\n"
            "        }}\n"
            "        case {8}: _a = _vio_daddr_{0}; _l = 1; _wr = 1; _vio_buf_{0}[0] = _vio_st_{0}; break;\n"
            "        case {9}: {{\n"
            "            _a = _vio_used_{0} + 4 + 8 * (_vio_used_idx_{0} % _vio_qnum_{0}); _l = 8; _wr = 1;\n"
            "            uint32_t _head = _vio_head_{0};\n"
            "            dcs_memcpy(_vio_buf_{0}, &_head, 4);\n"
            "            uint32_t _n = (_vio_type_{0} == {6}) ? _vio_written_{0} : 0;\n"
            "            dcs_memcpy(_vio_buf_{0} + 4, &_n, 4);\n"
            "            break;\n"
            "        }}\n"
            "        case {10}: {{\n"
            "            _a = _vio_used_{0} + 2; _l = 2; _wr = 1;\n"
            "            uint16_t _ni = (uint16_t)(_vio_used_idx_{0} + 1);\n"
            "            dcs_memcpy(_vio_buf_{0}, &_ni, 2);\n"
            "            break;\n"
            "        }}\n"
            "        default: break;\n"
            "        }}\n"
            "        if (_l) {{\n"
            "            _vio_mp_active_{0} = 1; _vio_mp_addr_{0} = _a; _vio_mp_len_{0} = _l;\n"
            "            _vio_mp_beat_{0} = 0; _vio_mp_write_{0} = _wr; _vio_mp_ackp_{0} = 0;\n"
            "        }}\n"
            "        }} // else if (_vio_phase_)\n"
            "    }}\n",
            _id, Q_AVAIL_IDX, Q_AVAIL_RING, Q_DESC, Q_HDR, Q_DATA, BLK_T_IN, (uint32_t) BLK_S_IOERR, Q_STATUS,
            Q_USED, Q_USED_IDX);
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_ack", 1) + "\n";
    c += "    const uint8_t _irq_out = _vio_int_" + std::to_string(_id) + " != 0;\n";
    c += "    " + genOutputWrite(O_IRQ, "_irq_out", 1) + "\n";
    c += "    " + genOutputWrite(O_M_ADR, "_m_adr", 32) + "\n";
    c += "    " + genOutputWrite(O_M_DAT, "_m_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_M_WE, "_m_we", 1) + "\n";
    c += "    " + genOutputWrite(O_M_SEL, "_m_sel", 4) + "\n";
    c += "    " + genOutputWrite(O_M_STB, "_m_stb", 1) + "\n";
    c += "    " + genOutputWrite(O_M_CYC, "_m_cyc", 1) + "\n";
    c += "}\n";
    return c;
}

std::string VirtioMmio::genDeinitCode() const {
    return "";
}

std::unique_ptr<Component> VirtioMmio::clone(const std::string &new_name) const {
    return std::make_unique<VirtioMmio>(new_name, _kind, _path);
}

uint32_t VirtioMmio::ioAccess(void *state, uint32_t off_lo, uint32_t off_hi, uint8_t *buf, uint32_t len,
                              uint32_t write) {
    auto *s = static_cast<State *>(state);
    const uint64_t off = ((uint64_t) off_hi << 32) | off_lo;
    if (off + len > s->image_size)
        return 0;
    if (write) {
        if (s->read_only || !s->fp)
            return 0;
        if (std::fseek(s->fp, (long) off, SEEK_SET) != 0)
            return 0;
        if (std::fwrite(buf, 1, len, s->fp) != len)
            return 0;
        std::fflush(s->fp);
        return 1;
    }
    if (s->fp) {
        if (std::fseek(s->fp, (long) off, SEEK_SET) != 0)
            return 0;
        return std::fread(buf, 1, len, s->fp) == len ? 1 : 0;
    }
    if (s->image) {
        std::memcpy(buf, s->image + off, len);
        return 1;
    }
    return 0;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_mmio", {{"device", "blk", {"blk", "console"}}, {"path", ""}},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-mmio modern 设备：从端口接 CPU，主端口经总线访问 virtqueue；blk 后端接宿主镜像或 ROM"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        const std::string dev = dsc::paramStr(p, "device", "blk");
        return std::make_unique<dsc::VirtioMmio>(n, dev == "console" ? dsc::VirtioMmio::Kind::Console
                                                                     : dsc::VirtioMmio::Kind::Blk,
                                                 dsc::paramStr(p, "path", ""));
    });
} // namespace
