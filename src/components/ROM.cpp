// ROM — 只读存储器实现（内存 / 文件 双模式，字节寻址）
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

    MappedFile(const std::filesystem::path &path) {
        _file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
        if (_file == INVALID_HANDLE_VALUE)
            throw std::runtime_error(std::format("无法打开 ROM 文件: {}", path.string()));

        LARGE_INTEGER li;
        GetFileSizeEx(_file, &li);

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

    MappedFile(const std::filesystem::path &path) {
        _fd = ::open(path.c_str(), O_RDONLY);
        if (_fd < 0)
            throw std::runtime_error(std::format("无法打开 ROM 文件: {}", path.string()));

        struct stat st;
        if (fstat(_fd, &st) < 0) {
            ::close(_fd);
            throw std::runtime_error(std::format("fstat 失败: {}", path.string()));
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
    setParam("addr_width", std::to_string(addr_width));
    setParam("data_width", std::to_string(data_width));
    setParam("read_latency", std::to_string(read_latency));

    addInput("addr", addr_width);
    addInput("clk", 1);
    addOutput("data_out", data_width, false, true);
}

// ============================================================
// 构造函数1：内存模式
// ============================================================
ROM::ROM(const std::string &name, int addr_width, int data_width, int read_latency) :
    SequentialComponent(name, "rom"), _addr_width(addr_width), _data_width(data_width), _depth(1 << addr_width),
    _read_latency(read_latency), _rd_stages(read_latency + 1) {

    initCommon(name, addr_width, data_width, read_latency);
    setParam("source_type", "mem");

    // 字节寻址：容量 = 2^addr_width 字节
    _mem.resize(_depth, 0);
    _data_ptr = _mem.data();
    _data_size = _mem.size();
}

// ============================================================
// 构造函数2：文件模式
// ============================================================
ROM::ROM(const std::string &name, int addr_width, int data_width, const std::filesystem::path &filepath,
         int read_latency) :
    SequentialComponent(name, "rom"), _addr_width(addr_width), _data_width(data_width), _depth(1 << addr_width),
    _read_latency(read_latency), _rd_stages(read_latency + 1), _filepath(filepath) {

    initCommon(name, addr_width, data_width, read_latency);
    setParam("source_type", "file");
    setParam("filepath", filepath.string());

    // 文件加载推迟到 prepare()
}

ROM::~ROM() = default;

bool ROM::prepare(std::string &error) {
    if (_filepath.empty())
        return true; // 内存模式，已分配好
    try {
        loadFile(_filepath);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

void ROM::loadFile(const std::filesystem::path &filepath) {
    // 优先尝试 mmap
    try {
        _mapped = std::make_unique<MappedFile>(filepath);
        _data_ptr = _mapped->addr;
        _data_size = std::min(_mapped->size, (size_t)_depth);
        return;
    } catch (const std::exception &) {
        _mapped.reset();
    }

    // mmap 失败，读入 _mem（字节寻址）
    _mem.resize(_depth, 0);
    std::ifstream f(filepath, std::ios::binary);
    if (!f)
        throw std::runtime_error(std::format("无法打开 ROM 文件: {}", filepath.string()));

    f.read((char *)_mem.data(), _mem.size());
    _data_size = std::min((size_t)f.gcount(), (size_t)_depth);
    _data_ptr = _mem.data();
}

// ============================================================
// C 代码生成（紧凑存储，d_bytes 步长）
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
    int d_bytes = byte_count(_data_width);
    std::string addr_mask = gen_mask(_addr_width);

    std::string clk_read = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";

    std::string code;
    code += std::format("// ROM: {}b × {}\n", _data_width, _depth);
    code += std::format("static void {}() {{\n", funcName_seq());
    code += "    " + gen_read_wire(a_nid, _addr_width, a_nw, "_addr") + "\n";
    code += "    " + clk_read + "\n";
    code += std::format("    _addr &= {};\n", addr_mask);

    // 读流水线
    if (_rd_stages > 1) {
        for (int i = _rd_stages - 1; i >= 1; i--)
            code += std::format("    _rom_addr_{0}[{1}] = _rom_addr_{0}[{2}];\n", id, i, i - 1);
    }
    if (_rd_stages > 0)
        code += std::format("    _rom_addr_{0}[0] = _addr;\n", id);

    // 读输出（字节寻址，边界检查：addr + d_bytes <= 容量则读取，否则返回 0）
    if (q_nid >= 0) {
        int rlast = _rd_stages - 1;
        std::string d_type = c_int_type(_data_width);
        std::string addr_expr = _rd_stages > 0
            ? std::format("_rom_addr_{}[{}]", id, rlast)
            : "_addr";

        code += std::format("    {} _dout = 0;\n", d_type);
        code += std::format("    if ((uint64_t){} + {} <= {}ULL) {{\n", addr_expr, d_bytes, _data_size);
        code += std::format("        dcs_memcpy(&_dout, _dcs_rom_p_{} + {}, {});\n", id, addr_expr, d_bytes);
        code += "    }\n";
        code += "    " + genOutputWrite(0, "_dout", _data_width) + "\n";
    }

    code += "}\n";
    return code;
}

std::unique_ptr<Component> ROM::clone(const std::string &n) const {
    if (!_filepath.empty()) {
        return std::make_unique<ROM>(n, _addr_width, _data_width, _filepath, _read_latency);
    } else {
        auto r = std::make_unique<ROM>(n, _addr_width, _data_width, _read_latency);
        std::memcpy(r->_mem.data(), _mem.data(), _mem.size());
        return r;
    }
}

} // namespace dsc
