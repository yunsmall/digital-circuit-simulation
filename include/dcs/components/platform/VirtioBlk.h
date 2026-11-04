// virtio-blk：块设备。介质由后端提供（文件、ROM、宿主注入的存储），
// 和 virtio-net 一样按名字注册后端，元件本身不碰文件。
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dcs/Component.h"
#include "dcs/components/platform/VirtioCommon.h"

namespace dsc {

class VirtioBlk : public SequentialComponent {
public:
    struct State; // 定义在 .cpp

    struct BackendOps {
        void *ctx = nullptr;
        // 后端上下文初始化/销毁（文件后端在 deinit 里关文件）
        void (*init)(void *ctx) = nullptr;
        void (*deinit)(void *ctx) = nullptr;
        void (*reset)(void *ctx) = nullptr;
        // 按 64 位字节偏移读写介质，返回 1 成功
        uint32_t (*read)(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) = nullptr;
        uint32_t (*write)(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) = nullptr;
        // 介质字节数，决定设备容量；没有 write 就是只读介质
        uint64_t size = 0;
    };

    // 后端工厂：prepare 时按 backend_opts 打开介质并返回 ops。容量和只读特性
    // 必须在这里就确定，makeSpec 生成 config 空间时要用。ctx 用来解析
    // backend_opts 里的相对路径（后端自己知道哪个选项是路径）
    using BackendFactory = BackendOps (*)(const std::map<std::string, std::string> &opts, Context &ctx,
                                          std::string &error);

    // 后端注册表：第三方在自己的 .cpp 里用 DCS_REGISTER_VIRTIO_BLK_BACKEND 自注册，
    // 元件的 backend 参数只存名字，JSON 往返后能自动恢复
    static void registerBackend(const std::string &name, BackendFactory factory);

    // 默认给一块 16MB 的纯内存盘，电路不用每次都提供镜像；要真实数据用
    // backend=file + backend_opts.path，dcs_soc 也可以用 --disk 覆盖
    explicit VirtioBlk(const std::string &name, const std::string &backend = "mem",
                       std::map<std::string, std::string> backend_opts = {});
    ~VirtioBlk() override;

    bool prepareImpl(Context &ctx, std::string &error) override;
    bool prepareForCompile(Context &ctx, std::string &error) override;
    void simInit() override;
    void simDeinit() override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    void setBackend(const BackendOps &ops);

    // 后端名字和私有选项随元件参数一起序列化，宿主加载 JSON 后照名字接上实现
    const std::string &backendName() const {
        return _backend;
    }
    const std::map<std::string, std::string> &backendOpts() const {
        return _backend_opts;
    }

    // 运行期换盘：dcs_soc 的 --disk 在加载后、编译前调用。内部走同一个 file
    // 后端工厂，重开文件并刷新容量/只读标志，不另开一条 I/O 路径
    bool setImagePath(Context &ctx, const std::string &path, std::string &error);

    // JIT 回调：state 指向 VirtioBlk::State
    static uint32_t ioAccess(void *state, uint32_t off_lo, uint32_t off_hi, uint8_t *buf, uint32_t len, uint32_t write);

private:
    virtio_gen::DeviceSpec makeSpec() const;
    bool installBackend(const std::string &name, const std::map<std::string, std::string> &opts, Context &ctx,
                        std::string &error);

    std::unique_ptr<State> _state;
    std::string _backend;
    std::map<std::string, std::string> _backend_opts;
    BackendOps _ops;
    // setBackend 注入的介质由宿主维护；deinit 之后只有后端工厂这条路径能重新打开
    bool _external_backend = false;
};

// 后端自注册：放在后端实现文件的全局作用域。
// 名字是字符串字面量，所以变量名用 __COUNTER__ 生成，不能拿 name 直接拼
#define DCS_VBLK_CAT2(a, b) a##b
#define DCS_VBLK_CAT(a, b) DCS_VBLK_CAT2(a, b)
#define DCS_REGISTER_VIRTIO_BLK_BACKEND(name, factory_expr)                                          \
    namespace {                                                                                      \
    [[maybe_unused]] const bool DCS_VBLK_CAT(_dcs_vblk_backend_, __COUNTER__) =                      \
            (::dsc::VirtioBlk::registerBackend(name, factory_expr), true);                           \
    }

} // namespace dsc
