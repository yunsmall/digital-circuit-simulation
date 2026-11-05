#include "dcs/components/platform/VirtioBlk.h"

#include "dcs/ComponentFactory.h"
#include "dcs/Context.h"
#include "dcs/net/BackendOpts.h"

#include <filesystem>
#include <format>
#include <map>

namespace dsc {

struct VirtioBlk::State {
    BackendOps ops;
    uint64_t image_size = 0;
    bool read_only = false;
};

namespace {

std::map<std::string, VirtioBlk::BackendFactory> &backendRegistry() {
    static std::map<std::string, VirtioBlk::BackendFactory> m;
    return m;
}

// JIT 侧回调：状态首成员是 BackendOps
// 复位回调名按设备类区分：JIT 符号表按名字索引，必须和 register 时用的字符串一致
constexpr const char *kResetSymbol = "dcs_virtio_blk_reset";

void resetThunk(void *ctx) {
    auto *s = static_cast<VirtioBlk::State *>(ctx);
    // 后端创建失败时 ctx 为空，不能指望后端实现自己防御
    if (s->ops.reset && s->ops.ctx)
        s->ops.reset(s->ops.ctx);
}

} // namespace

void VirtioBlk::registerBackend(const std::string &name, BackendFactory factory) {
    backendRegistry()[name] = factory;
}

VirtioBlk::VirtioBlk(const std::string &name, const std::string &backend,
                     std::map<std::string, std::string> backend_opts) :
    SequentialComponent(name, "virtio_blk"), _state(std::make_unique<State>()), _backend(backend),
    _backend_opts(std::move(backend_opts)) {
    setParam("backend", backend);
    // 和后端名字一样按扁平键记录，JSON 导出/回读能原样恢复
    for (auto &[k, v]: _backend_opts)
        setParam(std::string(kBackendOptsPrefix) + k, v);
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
    if (_state->ops.deinit && _state->ops.ctx) {
        _state->ops.deinit(_state->ops.ctx);
        _state->ops.ctx = nullptr;
    }
}

void VirtioBlk::setBackend(const BackendOps &ops) {
    _ops = ops;
    _state->ops = ops;
    _state->image_size = ops.size;
    // 没有写回调就是只读介质，feature 位和状态字节都要按只读处理
    _state->read_only = ops.read && !ops.write;
    _external_backend = true;
}

bool VirtioBlk::installBackend(const std::string &name, const std::map<std::string, std::string> &opts,
                               Context &ctx, std::string &error) {
    auto it = backendRegistry().find(name);
    if (it == backendRegistry().end()) {
        error = std::format("virtio_blk 请求的后端 '{}' 没有注册", name);
        return false;
    }
    BackendOps ops = it->second(opts, ctx, error);
    if (!error.empty())
        return false;
    // 换盘/重配时先释放旧介质，否则文件句柄会泄漏
    if (_state->ops.deinit && _state->ops.ctx)
        _state->ops.deinit(_state->ops.ctx);

    _ops = ops;
    _state->ops = ops;
    _state->image_size = ops.size;
    _state->read_only = ops.read && !ops.write;
    _external_backend = false;
    _backend = name;
    _backend_opts = opts;

    setParam("backend", name);
    for (auto p = _params.begin(); p != _params.end();) {
        if (p->first.rfind(kBackendOptsPrefix, 0) == 0)
            p = _params.erase(p);
        else
            ++p;
    }
    for (auto &[k, v]: _backend_opts)
        setParam(std::string(kBackendOptsPrefix) + k, v);
    return true;
}

bool VirtioBlk::setImagePath(Context &ctx, const std::string &path, std::string &error) {
    // 空路径表示沿用 JSON 里的默认值，不要把已有镜像关掉
    if (path.empty())
        return true;
    // --disk 是命令行参数，按工作目录解析，不能套用 JSON 的相对目录。
    // 转成绝对路径后 Context::resolvePath 不会再拼 source_dir
    std::error_code ec;
    const auto abs = std::filesystem::absolute(path, ec);
    const std::string resolved = ec ? path : abs.string();
    return installBackend("file", {{"path", resolved}}, ctx, error);
}

bool VirtioBlk::prepareImpl(Context &ctx, std::string &error) {
    // 外部后端（setBackend / 测试注入）已经声明了容量和读写语义时优先用它；
    // 否则按 backend 名字走注册表。文件/ROM 的打开逻辑都在各自后端里。
    // 不能只按 _ops.read 是否为空判断：外部后端漏填 read 时会被注册表后端静默覆盖
    if (_external_backend) {
        if (!_ops.read) {
            error = "外部注入的 virtio_blk 后端缺少 read 回调";
            return false;
        }
    } else if (!_ops.read && _backend != "none") {
        if (!installBackend(_backend, _backend_opts, ctx, error))
            return false;
    }
    _state->ops = _ops;
    _state->image_size = _ops.size;
    _state->read_only = _ops.read && !_ops.write;
    return true;
}

// deinit 关掉了介质并把 ops.ctx 置空；重新编译必须重开，否则 JIT 的
// dcs_virtio_io 会拿着空 ctx 调后端。外部注入的 BackendOps 由宿主负责重建
bool VirtioBlk::prepareForCompile(Context &ctx, std::string &error) {
    if (!_external_backend && _backend != "none" && !_state->ops.ctx)
        return installBackend(_backend, _backend_opts, ctx, error);
    return true;
}

void VirtioBlk::simInit() {
    // init 可能被重复调用（用户把 init 当重置用）：后端 init 不可重入，
    // 重复执行会泄漏它自己开的资源。deinit 会清掉标志，下一轮才重新执行
    if (_init_called)
        return;
    if (_state->ops.init)
        _state->ops.init(_state->ops.ctx);
    _init_called = true;
}

void VirtioBlk::simDeinit() {
    _init_called = false;
    if (_state->ops.deinit && _state->ops.ctx) {
        _state->ops.deinit(_state->ops.ctx);
        _state->ops.ctx = nullptr;
    }
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
    for (int i = 0; i < virtio_gen::IN_COUNT; i++)
        p.in_nets[i] = (int) inputs()[i]->netId();
    p.features = _state->read_only ? 32u : 0u;
    p.size_bytes = _state->image_size;
    p.state_decl = std::format(
            "#define _VIO_{0}_RO {1}u\n"
            "static uint32_t _vio_type_{0};\n"
            "static uint64_t _vio_sec_{0};\n"
            "static uint8_t _vio_status_byte_{0};\n"
            "extern uint32_t dcs_virtio_io(void *, uint32_t, uint32_t, uint8_t *, uint32_t, uint32_t);\n",
            p.id, _state->read_only ? 1u : 0u);
    p.init_code = std::format(
            "    _vio_status_byte_{0} = 0; _vio_type_{0} = 0; _vio_sec_{0} = 0;\n", p.id);
    p.read_stmt = std::format(
            "                uint32_t _ok = dcs_virtio_io(_vio_state_{0}, (uint32_t)_io_off,\n"
            "                                             (uint32_t)(_io_off >> 32), _vio_mp_buf_{0}, _c, 0);\n"
            "                if (!_ok) _vio_status_byte_{0} = 1;\n",
            p.id);
    p.write_stmt = std::format(
            "                    uint32_t _ok = dcs_virtio_io(_vio_state_{0}, (uint32_t)_io_off,\n"
            "                                                 (uint32_t)(_io_off >> 32), _vio_mp_buf_{0}, _c, 1);\n"
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
    // 外部注入的 BackendOps（介质）不随 clone 复制：clone 用于复合元件展开，
    // 复制会让两个设备共享同一个介质并在销毁时重复关闭
    return std::make_unique<VirtioBlk>(new_name, _backend, _backend_opts);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"virtio_blk", {{"backend", "mem"}, {"backend_opts", ""}},
     {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "m_dat_i", "m_ack_i"},
     {"wb_dat_o", "wb_ack_o", "irq", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Bus,
     "virtio-blk：从端口接 CPU，主端口经总线访问 virtqueue；介质由 backend 提供（file 后端用 backend_opts.path）", true},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::VirtioBlk>(n, dsc::paramStr(p, "backend", "mem"), dsc::takeBackendOpts(p));
    });
} // namespace
