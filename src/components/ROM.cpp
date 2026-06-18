// ROM — 只读存储器实现（hex / raw / mmap 三合一）
#include "dcs/components/ROM.h"
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dsc {

// ============================================================
// MappedFile — RAII 文件 mmap 封装
// ============================================================
struct ROM::MappedFile {
    const uint8_t *addr = nullptr;
    size_t size = 0;

#ifdef _WIN32
    HANDLE _file = INVALID_HANDLE_VALUE;
    HANDLE _mapping = NULL;

    MappedFile(const std::filesystem::path &path, size_t expected) {
        _file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
        if (_file == INVALID_HANDLE_VALUE)
            throw std::runtime_error(std::format("无法打开 ROM 文件: {}", path.string()));

        LARGE_INTEGER li;
        GetFileSizeEx(_file, &li);
        if ((size_t) li.QuadPart < expected)
            throw std::runtime_error(
                    std::format("ROM 文件太小: 需要 {} 字节, 实际 {} 字节", expected, (size_t) li.QuadPart));

        _mapping = CreateFileMappingW(_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!_mapping) {
            CloseHandle(_file);
            throw std::runtime_error(std::format("CreateFileMapping 失败: {}", path.string()));
        }

        addr = (const uint8_t *) MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, 0);
        if (!addr) {
            CloseHandle(_mapping);
            CloseHandle(_file);
            throw std::runtime_error(std::format("MapViewOfFile 失败: {}", path.string()));
        }
        size = (size_t) li.QuadPart;
    }

    ~MappedFile() {
        if (addr)
            UnmapViewOfFile(addr);
        if (_mapping)
            CloseHandle(_mapping);
        if (_file != INVALID_HANDLE_VALUE)
            CloseHandle(_file);
    }
#else
    int _fd = -1;

    MappedFile(const std::filesystem::path &path, size_t expected) {
        _fd = ::open(path.c_str(), O_RDONLY);
        if (_fd < 0)
            throw std::runtime_error(std::format("无法打开 ROM 文件: {}", path.string()));

        struct stat st;
        if (fstat(_fd, &st) < 0) {
            ::close(_fd);
            throw std::runtime_error(std::format("fstat 失败: {}", path.string()));
        }
        if ((size_t) st.st_size < expected) {
            ::close(_fd);
            throw std::runtime_error(
                    std::format("ROM 文件太小: 需要 {} 字节, 实际 {} 字节", expected, (size_t) st.st_size));
        }

        addr = (const uint8_t *) mmap(nullptr, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, _fd, 0);
        if (addr == MAP_FAILED) {
            ::close(_fd);
            throw std::runtime_error(std::format("mmap 失败: {}", path.string()));
        }
        size = (size_t) st.st_size;
    }

    ~MappedFile() {
        if (addr && addr != MAP_FAILED)
            munmap((void *) addr, size);
        if (_fd >= 0)
            ::close(_fd);
    }
#endif
};

// ============================================================
// 公共初始化
// ============================================================
void ROM::initCommon(const std::string &name, int addr_width, int data_width, int read_latency) {
    if (addr_width < 1 || addr_width > 12)
        throw std::invalid_argument("地址位宽必须在1-12之间");
    if (data_width < 1 || data_width > 64)
        throw std::invalid_argument("数据位宽必须在1-64之间");
    if (read_latency < 0 || read_latency > 15)
        throw std::invalid_argument("读延迟必须在0-15之间");

    setParam("addr_width", std::to_string(addr_width));
    setParam("data_width", std::to_string(data_width));
    setParam("read_latency", std::to_string(read_latency));

    addInput("addr", addr_width);
    addInput("clk", 1);
    addOutput("data_out", data_width, false, true);
}

// ============================================================
// 构造函数1：hex 字符串
// ============================================================
ROM::ROM(const std::string &name, int addr_width, int data_width, const std::string &initial_data, int read_latency) :
    SequentialComponent(name, "rom"), _addr_width(addr_width), _data_width(data_width), _depth(1 << addr_width),
    _read_latency(read_latency), _rd_stages(read_latency + 1), _initial_data(initial_data) {

    initCommon(name, addr_width, data_width, read_latency);
    setParam("initial_data", initial_data);
    setParam("source_type", "hex");

    parseHex(initial_data);
}

void ROM::parseHex(const std::string &hex) {
    _mem.resize(_depth * 16, 0);
    int d_bytes = (_data_width + 7) / 8;
    for (size_t i = 0; i < hex.length() && i / (2 * d_bytes) < (size_t) _depth; i += 2) {
        if (i + 1 < hex.length()) {
            size_t row = (i / 2) / d_bytes;
            size_t col = (i / 2) % d_bytes;
            char h[3] = {hex[i], hex[i + 1], 0};
            _mem[row * 16 + col] = (uint8_t) strtoul(h, nullptr, 16);
        }
    }
    _data_ptr = _mem.data();
}

// ============================================================
// 构造函数2：raw 字节数组
// ============================================================
ROM::ROM(const std::string &name, int addr_width, int data_width, const uint8_t *data, size_t size, int read_latency) :
    SequentialComponent(name, "rom"), _addr_width(addr_width), _data_width(data_width), _depth(1 << addr_width),
    _read_latency(read_latency), _rd_stages(read_latency + 1) {

    initCommon(name, addr_width, data_width, read_latency);
    setParam("source_type", "raw");

    loadRaw(data, size);
}

void ROM::loadRaw(const uint8_t *data, size_t size) {
    _mem.resize(_depth * 16, 0);
    int d_bytes = (_data_width + 7) / 8;
    size_t copy_bytes = (std::min) (size, _depth * (size_t) d_bytes);
    // 逐行拷贝：每行 d_bytes 有效数据，填充到 16 字节槽中
    for (size_t i = 0; i < copy_bytes; i++) {
        size_t row = i / d_bytes;
        size_t col = i % d_bytes;
        _mem[row * 16 + col] = data[i];
    }
    // 保存副本供 clone 使用
    _raw_copy.assign(data, data + size);
    _data_ptr = _mem.data();
}

// ============================================================
// 构造函数3：文件加载
// ============================================================
ROM::ROM(const std::string &name, int addr_width, int data_width, const std::filesystem::path &filepath,
         int read_latency, bool use_mmap) :
    SequentialComponent(name, "rom"), _addr_width(addr_width), _data_width(data_width), _depth(1 << addr_width),
    _read_latency(read_latency), _rd_stages(read_latency + 1), _filepath(filepath), _use_mmap(use_mmap) {

    initCommon(name, addr_width, data_width, read_latency);
    setParam("source_type", "file");
    setParam("filepath", filepath.string());
    setParam("use_mmap", use_mmap ? "1" : "0");

    // 文件加载推迟到 prepare()
}

bool ROM::prepare(std::string &error) {
    if (_filepath.empty())
        return true; // hex / raw 模式，已在构造函数中加载
    try {
        loadFile(_filepath, _use_mmap);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

void ROM::loadFile(const std::filesystem::path &filepath, bool use_mmap) {
    if (use_mmap) {
        // mmap 零拷贝：文件需为 16 字节/行格式（与 _mem 布局一致）
        size_t needed = _depth * 16;
        _mapped = std::make_unique<MappedFile>(filepath, needed);
        _data_ptr = _mapped->addr;
        // mmap 可能比 needed 大，没事，只读前 needed 字节
    }
    else {
        // 读入内存
        _mem.resize(_depth * 16, 0);
        std::ifstream f(filepath, std::ios::binary);
        if (!f)
            throw std::runtime_error(std::format("无法打开 ROM 文件: {}", filepath.string()));

        int d_bytes = (_data_width + 7) / 8;
        std::vector<uint8_t> buf(_depth * d_bytes, 0);
        f.read((char *) buf.data(), buf.size());
        size_t got = (size_t) f.gcount();
        for (size_t i = 0; i < got; i++) {
            size_t row = i / d_bytes;
            size_t col = i % d_bytes;
            _mem[row * 16 + col] = buf[i];
        }
        _data_ptr = _mem.data();
    }
}

ROM::~ROM() = default;

// ============================================================
// C 代码生成（与旧版 ROM 一致，仅指针来源不同）
// ============================================================
std::string ROM::genStateDecl() const {
    int id = _id;
    std::string code;
    code += std::format("static uint8_t* _dcs_rom_p_{0} = (uint8_t*)0x{1:X}ULL;\n", id,
                        reinterpret_cast<uintptr_t>(_data_ptr));
    code += std::format("static uint8_t  _dcs_rom_clk_{0};\n", id);
    if (_rd_stages > 0)
        code += std::format("static uint64_t _rom_addr_{0}[{1}];\n", id, _rd_stages);
    return code;
}

std::string ROM::genInitCode() const {
    int id = _id;
    std::string code;
    code += std::format("    _dcs_rom_clk_{0} = 0;\n", id);
    code += std::format("    dcs_memset(_rom_addr_{0}, 0, sizeof(_rom_addr_{0}));\n", id);
    return code;
}

std::string ROM::genFuncDef_seq() const {
    int id = _id;
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int clk_nid = inputs()[1]->netId();
    int q_nid = outputs()[0]->netId();
    int d_bytes = (_data_width + 7) / 8;
    std::string addr_mask = gen_mask(_addr_width);

    std::string clk_read = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";

    std::string code;
    code += std::format("// ROM: {}×{}\n", _addr_width, _data_width);
    code += std::format("static void {}() {{\n", funcName_seq());
    code += "    " + gen_read_wire(a_nid, _addr_width, a_nw, "_addr") + "\n";
    code += "    " + clk_read + "\n";
    code += std::format("    _addr &= {};\n", addr_mask);

    // 读流水线
    if (_rd_stages > 1) {
        code += "    // 读流水线移位\n";
        for (int i = _rd_stages - 1; i >= 1; i--)
            code += std::format("    _rom_addr_{0}[{1}] = _rom_addr_{0}[{2}];\n", id, i, i - 1);
    }
    if (_rd_stages > 0)
        code += std::format("    _rom_addr_{0}[0] = _addr;\n", id);

    // 读输出
    if (q_nid >= 0) {
        int rlast = _rd_stages - 1;
        std::string d_type = c_int_type(_data_width);
        if (_rd_stages > 0) {
            code += std::format("    uint8_t* _rslot = _dcs_rom_p_{0} + _rom_addr_{0}[{1}] * 16;\n", id, rlast);
        }
        else {
            code += std::format("    uint8_t* _rslot = _dcs_rom_p_{0} + _addr * 16;\n", id);
        }
        code += std::format("    {} _dout = 0;\n", d_type);
        code += std::format("    dcs_memcpy(&_dout, _rslot, {});\n", d_bytes);
        code += "    " + genOutputWrite(0, "_dout", _data_width) + "\n";
    }

    code += "}\n";
    return code;
}

std::unique_ptr<Component> ROM::clone(const std::string &n) const {
    if (_mapped || _use_mmap) {
        // 文件模式：重新 mmap/读文件
        return std::make_unique<ROM>(n, _addr_width, _data_width, _filepath, _read_latency, _use_mmap);
    }
    else if (!_raw_copy.empty()) {
        // raw 模式
        return std::make_unique<ROM>(n, _addr_width, _data_width, _raw_copy.data(), _raw_copy.size(), _read_latency);
    }
    else {
        // hex 模式
        return std::make_unique<ROM>(n, _addr_width, _data_width, _initial_data, _read_latency);
    }
}

} // namespace dsc
