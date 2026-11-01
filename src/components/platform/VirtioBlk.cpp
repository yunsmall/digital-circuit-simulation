#include "dcs/components/platform/VirtioBlk.h"

#include "dcs/ComponentFactory.h"

#include <cstdio>
#include <cstring>
#include <format>

namespace dsc {

struct VirtioBlk::State {
    BackendOps ops;
    std::FILE *fp = nullptr;
    uint64_t image_size = 0;
    bool read_only = false;
};

namespace {

uint32_t fileRead(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    auto *s = static_cast<VirtioBlk::State *>(ctx);
    if (!s->fp || off + len > s->image_size)
        return 0;
    if (std::fseek(s->fp, (long) off, SEEK_SET) != 0)
        return 0;
    return std::fread(buf, 1, len, s->fp) == len ? 1 : 0;
}

uint32_t fileWrite(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    auto *s = static_cast<VirtioBlk::State *>(ctx);
    if (off + len > s->image_size)
        return 0;
    if (s->read_only || !s->fp)
        return 0;
    if (std::fseek(s->fp, (long) off, SEEK_SET) != 0)
        return 0;
    if (std::fwrite(buf, 1, len, s->fp) != len)
        return 0;
    std::fflush(s->fp);
    return 1;
}

// JIT 侧回调：状态首成员是 BackendOps
// 复位回调名按设备类区分：JIT 符号表按名字索引，必须和 register 时用的字符串一致
constexpr const char *kResetSymbol = "dcs_virtio_blk_reset";

void resetThunk(void *ctx) {
    auto *s = static_cast<VirtioBlk::State *>(ctx);
    if (s->ops.reset)
        s->ops.reset(s->ops.ctx);
}

} // namespace

VirtioBlk::VirtioBlk(const std::string &name, const std::string &path) :
    SequentialComponent(name, "virtio_blk"), _state(std::make_unique<State>()), _path(path) {
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

VirtioBlk::~VirtioBlk() {
    if (_state->fp)
        std::fclose(_state->fp);
}

void VirtioBlk::setBackend(const BackendOps &ops) {
    _state->ops = ops;
    _state->image_size = ops.size;
    // 没有写回调就是只读介质，feature 位和状态字节都要按只读处理
    _state->read_only = ops.read && !ops.write;
}

bool VirtioBlk::prepare(std::string &error) {
    if (!_path.empty()) {
        _state->fp = std::fopen(_path.c_str(), "r+b");
        if (!_state->fp) {
            _state->fp = std::fopen(_path.c_str(), "rb");
            _state->read_only = true;
        }
        if (!_state->fp) {
            error = std::format("virtio_blk 无法打开镜像 '{}'", _path);
            return false;
        }
        std::fseek(_state->fp, 0, SEEK_END);
        _state->image_size = (uint64_t) std::ftell(_state->fp);
        std::fseek(_state->fp, 0, SEEK_SET);
    }
    if (!_state->ops.read) {
        _state->ops.ctx = _state.get();
        _state->ops.read = fileRead;
        _state->ops.write = fileWrite;
    }
    _prepared = true;
    return true;
}

void VirtioBlk::simInit() {
    if (_state->ops.init)
        _state->ops.init(_state->ops.ctx);
}

void VirtioBlk::simDeinit() {
    if (_state->ops.deinit)
        _state->ops.deinit(_state->ops.ctx);
}

std::vector<Component::JitSymbol> VirtioBlk::extraJitSymbols() const {
    return {
            {"dcs_virtio_io", reinterpret_cast<void *>(&VirtioBlk::ioAccess)},
            {kResetSymbol, reinterpret_cast<void *>(&resetThunk)},
            {"dcs_virtio_trace", reinterpret_cast<void *>(&dcs_virtio_trace)},
    };
}

uint32_t VirtioBlk::ioAccess(void *state, uint32_t off_lo, uint32_t off_hi, uint8_t *buf, uint32_t len,
                             uint32_t write) {
    auto *s = static_cast<State *>(state);
    const uint64_t off = ((uint64_t) off_hi << 32) | off_lo;
    if (write)
        return s->ops.write ? s->ops.write(s->ops.ctx, off, buf, len) : 0;
    return s->ops.read ? s->ops.read(s->ops.ctx, off, buf, len) : 0;
}

virtio_gen::DeviceSpec VirtioBlk::makeSpec() const {
    using namespace virtio_gen;
    BlkParams p;
    p.id = std::to_string(_id);
    p.func_name = funcName_seq();
    p.node_name = std::format("VirtioBlk {}", name());
    p.reset_symbol = kResetSymbol;
    for (int i = 0; i < 9; i++)
        p.in_nets.push_back((int) inputs()[i]->netId());
    p.features = _state->read_only ? 32u : 0u;
    p.size_bytes = _state->image_size;
    p.state_decl = std::format(
            "#define _VIO_{0}_RO {1}u\n"
            "static uint32_t _vio_type_{0}, _vio_sec_{0};\n"
            "static uint8_t _vio_status_byte_{0};\n"
            "extern uint32_t dcs_virtio_io(void *, uint32_t, uint32_t, uint8_t *, uint32_t, uint32_t);\n",
            p.id, _state->read_only ? 1u : 0u);
    p.init_code = std::format("    _vio_status_byte_{0} = 0;\n", p.id);
    p.read_stmt = std::format(
            "                uint32_t _ok = dcs_virtio_io(_vio_state_{0}, _io_off, 0, _vio_mp_buf_{0}, _c, 0);\n"
            "                if (!_ok) _vio_status_byte_{0} = 1;\n",
            p.id);
    p.write_stmt = std::format(
            "                    uint32_t _ok = dcs_virtio_io(_vio_state_{0}, _io_off, 0, _vio_mp_buf_{0}, _c, 1);\n"
            "                    if (!_ok) _vio_status_byte_{0} = _VIO_{0}_RO ? 3u : 1u;\n",
            p.id);
    p.out_write = [this](int i, const std::string &e, uint32_t w) { return genOutputWrite(i, e, w); };
    return makeBlkSpec(p);
}

std::string VirtioBlk::genStateDecl() const {
    return virtio_gen::genStateDecl(makeSpec());
}

std::string VirtioBlk::genFuncDef_seq() const {
    return virtio_gen::genSeq(makeSpec());
}

std::string VirtioBlk::genInitCode() const {
    return virtio_gen::genInitCode(makeSpec(), _state.get());
}

std::unique_ptr<Component> VirtioBlk::clone(const std::string &new_name) const {
    return std::make_unique<VirtioBlk>(new_name, _path);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_blk", {{"path", ""}},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-blk：从端口接 CPU，主端口经总线访问 virtqueue；镜像来自宿主文件或 ROM"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::VirtioBlk>(n, dsc::paramStr(p, "path", ""));
    });
} // namespace
