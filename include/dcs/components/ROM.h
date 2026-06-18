// ROM -- 只读存储器，支持 hex 字符串、raw 字节数组、文件 mmap 三种数据源
//
// 输入: addr（读地址）、clk（时钟）
// 输出: data_out（读数据）
// 读延迟: 地址改变后经过 read_latency 个 tick 数据在流水线末端有效
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
    // IStorage: 统一返回数据指针和大小
    const uint8_t *memPtr() const override { return _data_ptr; }
    size_t memSize() const override { return _depth * 16; }
    // ---- 构造方式1：hex 字符串（兼容旧版）--------
    // initial_data: 每 2 字符 = 1 字节（如 "AB" = 0xAB），低字节在前
    ROM(const std::string &name, int addr_width, int data_width, const std::string &initial_data = "",
        int read_latency = 0);

    // ---- 构造方式2：raw 字节数组（memcpy 拷贝）--------
    // data 指向原始字节，size 为字节数（超深截断，不足补零）
    ROM(const std::string &name, int addr_width, int data_width, const uint8_t *data, size_t size,
        int read_latency = 0);

    // ---- 构造方式3：文件加载（mmap 或 read）--------
    // filepath: 二进制文件路径
    // use_mmap: true=mmap 零拷贝，false=读入内存
    ROM(const std::string &name, int addr_width, int data_width, const std::filesystem::path &filepath,
        int read_latency, bool use_mmap);

    ~ROM() override;

    // 文件加载推迟到 prepare()（由 addComponent 调用），与 DLL 错误处理对齐
    bool prepare(std::string &error) override;

    int depth() const {
        return _depth;
    }
    int addrWidth() const {
        return _addr_width;
    }
    int dataWidth() const {
        return _data_width;
    }
    const uint8_t *data() const {
        return _data_ptr;
    }

    std::string genStructDef() const override {
        return "";
    }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    void initCommon(const std::string &name, int addr_width, int data_width, int read_latency);
    void parseHex(const std::string &hex);
    void loadRaw(const uint8_t *data, size_t size);
    void loadFile(const std::filesystem::path &filepath, bool use_mmap);

    int _addr_width, _data_width, _depth;
    int _read_latency, _rd_stages;
    std::string _initial_data; // hex 字符串（clone 用）
    std::vector<uint8_t> _raw_copy; // raw 字节拷贝（clone 用）
    std::filesystem::path _filepath; // 文件路径（clone 用）
    bool _use_mmap = false; // 是否 mmap 模式

    std::vector<uint8_t> _mem; // hex/raw 模式：自有存储
    const uint8_t *_data_ptr = nullptr; // 统一数据指针（指向 _mem 或 mmap 地址）

    // mmap 资源（RAII 内部类）
    struct MappedFile;
    std::unique_ptr<MappedFile> _mapped;
};

} // namespace dsc
