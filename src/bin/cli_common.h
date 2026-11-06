#pragma once
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/Context.h>
#include <dcs/util/PathList.h>

// 两个 CLI（dcs / dcs_soc）共用的加载与参数解析。放在 src/bin 而不是 dcs 库：
// 这些是命令行入口的约定（输出格式、查找路径优先级），不是仿真库的接口
namespace dcs_cli {

// --lib-path 在前，环境变量 DCS_LIB_PATH（PATH 风格）在后。
// 必须在任何 fromJson 之前调用，电路 JSON 的 definitions 会用到查找路径
inline void configureLibrary(dsc::Context &ctx, const std::vector<std::string> &lib_paths) {
    for (auto &p: lib_paths)
        ctx.library().addSearchPath(p);
    if (const char *env = std::getenv("DCS_LIB_PATH"))
        for (auto &p: dsc::splitPathList(env))
            ctx.library().addSearchPath(p);
}

// 读 JSON → 建电路。失败时打印原因并返回空
inline std::unique_ptr<dsc::Circuit> loadCircuit(const std::string &path, dsc::Context &ctx) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << std::format("错误: 无法打开 '{}'\n", path);
        return nullptr;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string error;
    auto circuit =
            dsc::Circuit::fromJson(oss.str(), error, std::filesystem::path(path).parent_path().string(), &ctx);
    if (!circuit) {
        std::cerr << std::format("JSON 错误: {}\n", error);
        return nullptr;
    }
    std::cout << std::format("电路: {} 线网, {} 元件\n", circuit->netCount(), circuit->componentCount());
    return circuit;
}

// --dump-mem 的 addr:len（十六进制）。用 from_chars 而不是 stoull：非法输入
// 要变成一条错误信息，不能让异常穿到 main 触发 terminate
inline bool parseAddrLen(const std::string &spec, size_t &addr, size_t &len, std::string &error) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos) {
        error = "--dump-mem 格式应为 addr:len（十六进制）";
        return false;
    }
    auto parse = [](std::string_view s, size_t &out) {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s.remove_prefix(2);
        uint64_t v = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
        if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size())
            return false;
        // 32 位宿主的 size_t 装不下 64 位地址，静默截断会让用户拿到错误地址
        if (v > (uint64_t) SIZE_MAX)
            return false;
        out = (size_t) v;
        return true;
    };
    const std::string_view view(spec);
    if (!parse(view.substr(0, colon), addr) || !parse(view.substr(colon + 1), len)) {
        error = "--dump-mem 的 addr/len 必须是十六进制整数: " + spec;
        return false;
    }
    // addr+len 溢出会让 dump 打印的地址回绕，必须在这里拦住
    if (addr > SIZE_MAX - len) {
        error = "--dump-mem 的 addr + len 溢出 size_t";
        return false;
    }
    return true;
}

template <class T>
T *findFirstComponent(dsc::Circuit &c) {
    for (auto &comp: c.components())
        if (auto *p = dynamic_cast<T *>(comp.get()))
            return p;
    return nullptr;
}

// 地址/数值的通用解析：十进制，或 0x 开头的十六进制
inline bool parseU64(const std::string &spec, uint64_t &out) {
    std::string_view s(spec);
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    if (s.empty())
        return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out, base);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

// --load FILE@ADDR：把原始字节放到 RAM 的物理地址。文件名里可能有 '@'，
// 所以从最后一个 '@' 拆
inline bool parseLoadSpec(const std::string &spec, std::string &path, size_t &addr, std::string &error) {
    const auto at = spec.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == spec.size()) {
        error = "--load 格式应为 FILE@ADDR: " + spec;
        return false;
    }
    path = spec.substr(0, at);
    uint64_t v = 0;
    if (!parseU64(spec.substr(at + 1), v) || v > (uint64_t) SIZE_MAX) {
        error = "--load 的地址必须是十进制或 0x 十六进制整数: " + spec;
        return false;
    }
    addr = (size_t) v;
    return true;
}

// --set NET=VALUE：值最多 128 位。十六进制按右对齐写进 16 字节槽，
// 十进制走 uint64；目的是给 CPU 的 start_pc/reset 这类输入线网一个入口
inline bool parseWireSet(const std::string &spec, std::string &net, std::vector<uint8_t> &bytes,
                         std::string &error) {
    const auto eq = spec.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == spec.size()) {
        error = "--set 格式应为 NET=VALUE: " + spec;
        return false;
    }
    net = spec.substr(0, eq);
    std::string value = spec.substr(eq + 1);

    bytes.assign(16, 0);
    std::string_view digits(value);
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        digits.remove_prefix(2);
    else {
        uint64_t v = 0;
        if (!parseU64(value, v)) {
            error = "--set 的值必须是十进制或 0x 十六进制整数: " + spec;
            return false;
        }
        for (int i = 0; i < 8; i++)
            bytes[(size_t) i] = (uint8_t) (v >> (8 * i));
        return true;
    }
    if (digits.empty() || digits.size() > 32) {
        error = "--set 的十六进制值必须在 1..32 位数字之间（128 位）: " + spec;
        return false;
    }
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < digits.size(); i++) {
        const int d = hex(digits[digits.size() - 1 - i]);
        if (d < 0) {
            error = "--set 的值不是合法的十六进制数: " + spec;
            return false;
        }
        if (i % 2 == 0)
            bytes[i / 2] = (uint8_t) d;
        else
            bytes[i / 2] |= (uint8_t) (d << 4);
    }
    return true;
}

// Circuit::deinit() 把 simDeinit/DLL 销毁时的异常留给显式调用方；CLI 的
// 结束路径不能让异常穿出 main（会 std::terminate），统一在这里报告。
// 返回 0 表示清理成功，非 0 表示有清理错误
inline int deinitIgnoringErrors(dsc::Circuit &circuit) {
    try {
        circuit.deinit();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("清理错误: {}\n", e.what());
    } catch (...) {
        std::cerr << "清理错误: 未知异常\n";
    }
    return 1;
}

// ---- --help 的中文折行 ----
// cxxopts 自带的折行按字节数数，会在 UTF-8 序列中间下刀，中文帮助在换行处变乱码。
// 这里关掉它的折行（set_width 给一个大值），在输出层按显示列重排：
// ASCII 算 1 列，其它码点算 2 列；只在 UTF-8 码点边界断开。

inline std::size_t utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

inline std::size_t displayWidth(std::string_view s) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char) s[i];
        std::size_t len = utf8CharLen(c);
        if (i + len > s.size()) len = 1;
        width += (c < 0x80) ? 1 : 2;
        i += len;
    }
    return width;
}

// 把一段文字折成不超过 width 显示列的行；续行用 indent 个空格缩进。
// 优先在空格处断，没有空格就按码点边界断
inline std::string wrapDisplayText(const std::string &text, std::size_t width, std::size_t indent) {
    std::string out;
    std::size_t seg = 0, i = 0, col = 0;
    std::size_t last_space = std::string::npos, last_space_col = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char) text[i];
        std::size_t len = utf8CharLen(c);
        if (i + len > text.size()) len = 1;
        const std::size_t w = (c < 0x80) ? 1 : 2;
        if (c == ' ') {
            last_space = i;
            last_space_col = col;
        }
        if (col + w > width && i > seg) {
            std::size_t cut = i, cut_col = col;
            if (last_space != std::string::npos && last_space > seg) {
                cut = last_space;
                cut_col = last_space_col;
            }
            out.append(text, seg, cut - seg);
            out.push_back('\n');
            out.append(indent, ' ');
            i = cut;
            if (i < text.size() && text[i] == ' ') ++i;
            seg = i;
            col = 0;
            last_space = std::string::npos;
            (void) cut_col;
            continue;
        }
        col += w;
        i += len;
    }
    out.append(text, seg, std::string::npos);
    return out;
}

// 重排 cxxopts 的 help：带"选项 + 两空格 + 描述"的行按描述列折行，
// 其余超宽行按整行折。输入必须是没被 cxxopts 折过的（宽度设得很大）
inline std::string formatHelp(const std::string &help, std::size_t width = 100) {
    std::istringstream in(help);
    std::string out, line;
    while (std::getline(in, line)) {
        if (displayWidth(line) <= width) {
            out += line;
            out.push_back('\n');
            continue;
        }
        // 第一个非行首空格后出现的"两个连续空格"是选项列与描述列的分界
        std::size_t first = line.find_first_not_of(' ');
        std::size_t desc = std::string::npos;
        for (std::size_t i = (first == std::string::npos ? 0 : first); i + 1 < line.size(); ++i) {
            if (line[i] == ' ' && line[i + 1] == ' ' && (i + 2 >= line.size() || line[i + 2] != ' ')) {
                desc = i;
                break;
            }
        }
        if (desc != std::string::npos && desc + 2 < line.size()) {
            std::size_t text_pos = line.find_first_not_of(' ', desc);
            if (text_pos != std::string::npos) {
                const std::string prefix = line.substr(0, desc);
                const std::size_t prefix_width = displayWidth(prefix);
                out += prefix;
                out += wrapDisplayText(line.substr(text_pos), width > prefix_width ? width - prefix_width : 10,
                                       prefix_width);
                out.push_back('\n');
                continue;
            }
        }
        // 没有描述列（usage/分组标题等）：整行折，续行保持原缩进
        const std::size_t indent = (first == std::string::npos ? 0 : first);
        out += wrapDisplayText(line, width, indent);
        out.push_back('\n');
    }
    return out;
}

} // namespace dcs_cli
