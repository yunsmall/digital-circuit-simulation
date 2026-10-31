// virtio-blk 的另一种形态：介质就是设备自己的存储（实现 IStorage）。
//
// 和 VirtioBlk 的区别只在介质访问：
//   VirtioBlk    —— 宿主文件或 BackendOps 回调，介质在设备外部
//   VirtioBlkMem —— 设备自己持有的字节数组，JIT 里直接 memcpy，零回调
//
// 两者共用 VirtioCommon::makeBlkSpec 的请求解析（请求头、数据段、状态字节、
// used 环），只有读写介质的那两条语句不同。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dcs/Component.h"
#include "dcs/IStorage.h"
#include "dcs/components/platform/VirtioCommon.h"

namespace dsc {

class VirtioBlkMem : public SequentialComponent, public IStorage {
public:
    // size_bytes 向下取整到 512 字节扇区
    explicit VirtioBlkMem(const std::string &name, uint64_t size_bytes = 1024u * 1024u);
    ~VirtioBlkMem() override;

    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // ---- IStorage：仅供仿真框架装载/检查内容，元件禁止调用 ----
    const uint8_t *readOnlyMemPtr() const override { return _mem.data(); }
    uint8_t *writableMemPtr() override { return _mem.data(); }
    size_t memSize() const override { return _mem.size(); }

private:
    virtio_gen::DeviceSpec makeSpec() const;

    std::vector<uint8_t> _mem;
};

} // namespace dsc
