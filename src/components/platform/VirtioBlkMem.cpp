#include "dcs/components/platform/VirtioBlkMem.h"

#include "dcs/ComponentFactory.h"

#include <format>

namespace dsc {

namespace {
// 设备没有外部后端，但 JIT 里 status 写 0 会调 reset，需要有个符号。
// 名字按设备类区分：JIT 符号表按名字索引，必须和 register 时用的字符串一致
constexpr const char *kResetSymbol = "dcs_virtio_blk_mem_reset";

void noopReset(void *) {}
} // namespace

VirtioBlkMem::VirtioBlkMem(const std::string &name, uint64_t size_bytes) :
    SequentialComponent(name, "virtio_blk_mem") {
    setParam("size_kb", std::to_string(size_bytes / 1024));
    _mem.assign((size_t) (size_bytes & ~511ull), 0);
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

VirtioBlkMem::~VirtioBlkMem() = default;

bool VirtioBlkMem::prepare(std::string & /*error*/) {
    return true;
}

std::vector<Component::JitSymbol> VirtioBlkMem::extraJitSymbols() const {
    return {{kResetSymbol, reinterpret_cast<void *>(&noopReset)},
            {"dcs_virtio_trace", reinterpret_cast<void *>(&dcs_virtio_trace)}};
}

virtio_gen::DeviceSpec VirtioBlkMem::makeSpec() const {
    using namespace virtio_gen;
    BlkParams p;
    p.id = std::to_string(_id);
    p.func_name = funcName_seq();
    p.node_name = std::format("VirtioBlkMem {}", name());
    p.reset_symbol = kResetSymbol;
    for (int i = 0; i < 9; i++)
        p.in_nets.push_back((int) inputs()[i]->netId());
    p.features = 0;
    p.size_bytes = _mem.size();
    p.state_decl = std::format(
            "static uint8_t *_vio_mem_{0};\n"
            "static unsigned long long _vio_mem_size_{0};\n"
            "static uint32_t _vio_type_{0}, _vio_sec_{0};\n"
            "static uint8_t _vio_status_byte_{0};\n",
            p.id);
    // 介质就是设备自己的存储，直接读写，不经过任何回调或总线
    p.init_code = std::format(
            "    _vio_mem_{0} = (uint8_t *)0x{1:X}ULL;\n"
            "    _vio_mem_size_{0} = {2}ull;\n"
            "    _vio_status_byte_{0} = 0;\n",
            p.id, (unsigned long long) reinterpret_cast<uintptr_t>(_mem.empty() ? nullptr : _mem.data()), _mem.size());
    p.read_stmt = std::format(
            "                if ((unsigned long long)(_io_off + _c) <= _vio_mem_size_{0})\n"
            "                    dcs_memcpy(_vio_mp_buf_{0}, _vio_mem_{0} + _io_off, _c);\n"
            "                else _vio_status_byte_{0} = 1;\n",
            p.id);
    p.write_stmt = std::format(
            "                    if ((unsigned long long)(_io_off + _c) <= _vio_mem_size_{0})\n"
            "                        dcs_memcpy(_vio_mem_{0} + _io_off, _vio_mp_buf_{0}, _c);\n"
            "                    else _vio_status_byte_{0} = 1;\n",
            p.id);
    p.out_write = [this](int i, const std::string &e, uint32_t w) { return genOutputWrite(i, e, w); };
    return makeBlkSpec(p);
}

std::string VirtioBlkMem::genStateDecl() const {
    return virtio_gen::genStateDecl(makeSpec());
}

std::string VirtioBlkMem::genFuncDef_seq() const {
    return virtio_gen::genSeq(makeSpec());
}

std::string VirtioBlkMem::genInitCode() const {
    return virtio_gen::genInitCode(makeSpec(), nullptr);
}

std::unique_ptr<Component> VirtioBlkMem::clone(const std::string &new_name) const {
    auto m = std::make_unique<VirtioBlkMem>(new_name, _mem.size());
    m->_mem = _mem;
    return m;
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_blk_mem", {{"size_kb", "1024"}},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-blk：介质是设备自带存储，JIT 直接读写；框架可用 IStorage 装载镜像"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        const uint64_t kb = (uint64_t) dsc::paramInt(p, "size_kb", 1024);
        return std::make_unique<dsc::VirtioBlkMem>(n, kb * 1024u);
    });
} // namespace
