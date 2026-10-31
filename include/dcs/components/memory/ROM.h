// ROM — 只读存储器
//
// 输入: addr（读地址）、clk（时钟）
// 输出: data_out（读数据）
// 读延迟: 地址改变后经过 read_latency 个 tick 数据在流水线末端有效
//
// 两种数据源:
//   内存模式 — 内部紧凑存储（depth × d_bytes），通过 IStorage 填入数据
//   文件模式 — 从二进制文件加载（紧凑原始字节），优先 mmap，失败回退 read
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "dcs/Component.h"
#include "dcs/IStorage.h"

namespace dsc {

class ROM : public SequentialComponent, public IStorage {
public:
    // IStorage: 紧凑存储，外部可直接读写
    const uint8_t *readOnlyMemPtr() const override { return _data_ptr; }
    uint8_t *writableMemPtr() override { return _mapped ? nullptr : _mem.data(); }
    size_t memSize() const override { return _data_size; }

    // ---- 构造方式1：内存模式 ----
    // 内部分配 depth × d_bytes 全零存储，调用者通过 writableMemPtr()/memSize() 填入数据
    ROM(const std::string &name, int addr_width, int data_width, int read_latency = 0);

    // ---- 构造方式2：文件模式 ----
    // 从二进制文件加载（紧凑原始字节），优先 mmap 零拷贝，失败回退 read
    // 文件加载推迟到 prepare() 中执行
    ROM(const std::string &name, int addr_width, int data_width, const std::filesystem::path &filepath,
        int read_latency = 0);

    ~ROM() override;

    bool prepare(std::string &error) override;

    int depth() const { return _depth; }
    int addrWidth() const { return _addr_width; }
    int dataWidth() const { return _data_width; }
    const uint8_t *data() const { return _data_ptr; }
    uint8_t *data() { return _mem.data(); } // 仅内存模式可写

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    void initCommon(const std::string &name, int addr_width, int data_width, int read_latency);
    void loadFile(const std::filesystem::path &filepath);

    int _addr_width, _data_width, _depth;
    int _read_latency, _rd_stages;
    std::filesystem::path _filepath; // 文件模式：文件路径（clone 用）

    std::vector<uint8_t> _mem; // 内存模式 / 文件 read 模式：自有存储
    const uint8_t *_data_ptr = nullptr; // 统一数据指针（指向 _mem 或 mmap 地址）
    size_t _data_size = 0; // IStorage: 实际有效数据字节数

    // mmap 资源（RAII 内部类）
    struct MappedFile;
    std::unique_ptr<MappedFile> _mapped;
};

} // namespace dsc
