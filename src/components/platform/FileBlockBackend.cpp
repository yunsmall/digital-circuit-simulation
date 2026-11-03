// virtio-blk 的文件后端：把宿主镜像文件映射进内存再当介质用。
// 元件本身不认识文件；换文件、接 ROM、接宿主内存都只换这里的实现。
#include "dcs/Context.h"
#include "dcs/components/platform/VirtioBlk.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <map>
#include <new>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

struct FileCtx {
    uint8_t *base = nullptr;
    uint64_t size = 0;
    bool read_only = false;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int fd = -1;
#endif
};

uint32_t fileRead(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    auto *c = static_cast<FileCtx *>(ctx);
    // 先减后比，避免 off + len 在 64 位上回绕骗过边界检查
    if (!c->base || off > c->size || len > c->size - off)
        return 0;
    std::memcpy(buf, c->base + off, len);
    return 1;
}

uint32_t fileWrite(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    auto *c = static_cast<FileCtx *>(ctx);
    if (!c->base || c->read_only || off > c->size || len > c->size - off)
        return 0;
    std::memcpy(c->base + off, buf, len);
    return 1;
}

void fileDeinit(void *ctx) {
    auto *c = static_cast<FileCtx *>(ctx);
    if (!c)
        return;
#ifdef _WIN32
    if (c->base)
        UnmapViewOfFile(c->base);
    if (c->mapping)
        CloseHandle(c->mapping);
    if (c->file != INVALID_HANDLE_VALUE)
        CloseHandle(c->file);
#else
    if (c->base)
        munmap(c->base, (size_t) c->size);
    if (c->fd >= 0)
        close(c->fd);
#endif
    delete c;
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string &s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}
#endif

dsc::VirtioBlk::BackendOps fileBackend(const std::map<std::string, std::string> &opts, dsc::Context &context,
                                       std::string &error) {
    auto it = opts.find("path");
    if (it == opts.end() || it->second.empty()) {
        error = "virtio_blk 的 file 后端需要 backend_opts.path";
        return {};
    }
    // 路径语义由后端自己决定：相对路径按当前 JSON/蓝图所在目录解析
    const std::string path = context.resolvePath(it->second);

    auto *ctx = new (std::nothrow) FileCtx();
    if (!ctx) {
        error = "virtio_blk 无法分配文件后端上下文";
        return {};
    }

#ifdef _WIN32
    const std::wstring wpath = utf8ToWide(path);
    // --disk 覆盖时旧盘还映射着同一个文件，必须允许读写共享，否则 Windows 会以
    // ERROR_SHARING_VIOLATION 拒绝第二次打开
    ctx->file = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (ctx->file == INVALID_HANDLE_VALUE) {
        ctx->file = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ctx->read_only = true;
    }
    if (ctx->file == INVALID_HANDLE_VALUE) {
        error = std::format("virtio_blk 无法打开镜像 '{}' (Win32 error {})", path, (unsigned long) GetLastError());
        delete ctx;
        return {};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(ctx->file, &size)) {
        error = std::format("virtio_blk 无法读取镜像大小 '{}'", path);
        fileDeinit(ctx);
        return {};
    }
    ctx->size = (uint64_t) size.QuadPart;
    if (ctx->size > 0) {
        ctx->mapping = CreateFileMappingW(ctx->file, nullptr, ctx->read_only ? PAGE_READONLY : PAGE_READWRITE,
                                          0, 0, nullptr);
        if (!ctx->mapping) {
            error = std::format("virtio_blk 无法映射镜像 '{}' (Win32 error {})", path,
                                (unsigned long) GetLastError());
            fileDeinit(ctx);
            return {};
        }
        ctx->base = static_cast<uint8_t *>(MapViewOfFile(
                ctx->mapping, ctx->read_only ? FILE_MAP_READ : (FILE_MAP_READ | FILE_MAP_WRITE), 0, 0, 0));
        if (!ctx->base) {
            error = std::format("virtio_blk 无法映射镜像 '{}' (Win32 error {})", path,
                                (unsigned long) GetLastError());
            fileDeinit(ctx);
            return {};
        }
    }
#else
    ctx->fd = open(path.c_str(), O_RDWR);
    if (ctx->fd < 0) {
        ctx->fd = open(path.c_str(), O_RDONLY);
        ctx->read_only = true;
    }
    if (ctx->fd < 0) {
        error = std::format("virtio_blk 无法打开镜像 '{}': {}", path, std::strerror(errno));
        delete ctx;
        return {};
    }
    struct stat st{};
    if (fstat(ctx->fd, &st) != 0) {
        error = std::format("virtio_blk 无法读取镜像大小 '{}': {}", path, std::strerror(errno));
        fileDeinit(ctx);
        return {};
    }
    ctx->size = (uint64_t) st.st_size;
    if (ctx->size > 0) {
        void *p = mmap(nullptr, (size_t) ctx->size, ctx->read_only ? PROT_READ : (PROT_READ | PROT_WRITE),
                       MAP_SHARED, ctx->fd, 0);
        if (p == MAP_FAILED) {
            error = std::format("virtio_blk 无法映射镜像 '{}': {}", path, std::strerror(errno));
            fileDeinit(ctx);
            return {};
        }
        ctx->base = static_cast<uint8_t *>(p);
    }
#endif

    dsc::VirtioBlk::BackendOps ops;
    ops.ctx = ctx;
    ops.read = &fileRead;
    ops.write = ctx->read_only ? nullptr : &fileWrite;
    ops.deinit = &fileDeinit;
    ops.size = ctx->size;
    return ops;
}

} // namespace

// 链接到 dcs_lib 就自动生效，JSON 里写 "backend": "file" 即可
DCS_REGISTER_VIRTIO_BLK_BACKEND("file", &fileBackend);
