#include "dcs/components/platform/VirtioBlkMem.h"

#include "dcs/ComponentFactory.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <stdexcept>

namespace dsc {

namespace {
// 设备没有外部后端，但 JIT 里 status 写 0 会调 reset，需要有个符号。
// 名字按设备类区分：JIT 符号表按名字索引，必须和 register 时用的字符串一致
constexpr const char *kResetSymbol = "dcs_virtio_blk_mem_reset";

void noopReset(void *) {}
} // namespace

VirtioBlkMem::VirtioBlkMem(const std::string &name, uint64_t size_bytes) :
    SequentialComponent(name, "virtio_blk_mem"), _size_bytes(size_bytes & ~511ull) {
    // 32 位宿主装不下 4 GiB 介质：size_t 截断后设备会报 0 容量，用户看到的是空盘。
    // 64 位宿主上这个比较恒假，用条件编译避免 -Wtautological
#if SIZE_MAX < UINT64_MAX
    if (_size_bytes > (uint64_t) SIZE_MAX)
        throw std::invalid_argument("size_bytes 超出 32 位宿主的可分配范围");
#endif
    // 导出对齐后的精确字节数：size_kb 向下取整，1536 字节会导成 1 KB，
    // 回读后容量缩水；size_bytes 是唯一能精确往返的口径
    setParam("size_bytes", std::to_string(_size_bytes));
    addInput("clk", 1, PinEdge::Rising);
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

bool VirtioBlkMem::prepareImpl(Context &, std::string & /*error*/) {
    allocateMem();
    return true;
}

void VirtioBlkMem::allocateMem() {
    const size_t bytes = (size_t) _size_bytes;
    if (_mem.size() != bytes)
        _mem.assign(bytes, 0);
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
    for (int i = 0; i < virtio_gen::IN_COUNT; i++)
        p.in_nets[i] = (int) inputs()[i]->netId();
    p.features = 0;
    p.size_bytes = _mem.size();
    p.state_decl = std::format(
            "static uint8_t *_vio_mem_{0};\n"
            "static unsigned long long _vio_mem_size_{0};\n"
            "static uint32_t _vio_type_{0};\n"
            "static uint64_t _vio_sec_{0};\n"
            "static uint8_t _vio_status_byte_{0};\n",
            p.id);
    // 介质就是设备自己的存储，直接读写，不经过任何回调或总线
    p.init_code = std::format(
            "    _vio_mem_{0} = (uint8_t *)0x{1:X}ULL;\n"
            "    _vio_mem_size_{0} = {2}ull;\n"
            "    _vio_status_byte_{0} = 0; _vio_type_{0} = 0; _vio_sec_{0} = 0;\n",
            p.id, (unsigned long long) reinterpret_cast<uintptr_t>(_mem.empty() ? nullptr : _mem.data()), _mem.size());
    p.read_stmt = std::format(
            "                if (_io_off <= _vio_mem_size_{0} && _c <= _vio_mem_size_{0} - _io_off)\n"
            "                    dcs_memcpy(_vio_mp_buf_{0}, _vio_mem_{0} + _io_off, _c);\n"
            "                else _vio_status_byte_{0} = 1;\n",
            p.id);
    p.write_stmt = std::format(
            "                    if (_io_off <= _vio_mem_size_{0} && _c <= _vio_mem_size_{0} - _io_off)\n"
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
    auto m = std::make_unique<VirtioBlkMem>(new_name, _size_bytes);
    m->_mem = _mem;
    return m;
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_blk_mem", {{"size_kb", "1024"}, {"size_bytes", ""}},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-blk：介质是设备自带存储，JIT 直接读写；框架可用 IStorage 装载镜像"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        // size_bytes 优先：导出的 JSON 用它，能精确还原非整 KB 的容量
        const std::string sb = dsc::paramStr(p, "size_bytes", "");
        if (!sb.empty()) {
            uint64_t bytes = 0;
            auto [ptr, ec] = std::from_chars(sb.data(), sb.data() + sb.size(), bytes);
            // 构造会把容量向下对齐到 512 字节扇区，小于 512 的输入会变成 0 容量
            if (ec != std::errc{} || ptr != sb.data() + sb.size() || bytes < 512 || bytes > 4ull * 1024 * 1024 * 1024)
                throw std::invalid_argument("size_bytes 必须在 512..4294967296 之间（按 512 字节扇区对齐）");
            return std::make_unique<dsc::VirtioBlkMem>(n, bytes);
        }
        const int kb = dsc::paramInt(p, "size_kb", 1024);
        // 负数转成 uint64 会回绕成天文数字，最后只在 assign 里炸出一句实现细节；
        // 上下限在这里挡掉，报错才说得清是参数问题
        if (kb <= 0 || kb > 4 * 1024 * 1024)
            throw std::invalid_argument(std::format("size_kb 必须在 1..{} 之间", 4 * 1024 * 1024));
        return std::make_unique<dsc::VirtioBlkMem>(n, (uint64_t) kb * 1024u);
    });
} // namespace
