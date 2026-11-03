// virtio-blk 的纯内存后端：按 backend_opts.size_mb 分配一块零初始化的内存盘。
// 不需要宿主文件，适合测试、ramdisk 和"镜像由外部灌入"的场景。
#include "dcs/Context.h"
#include "dcs/components/platform/VirtioBlk.h"

#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace {

struct MemCtx {
    std::vector<uint8_t> data;
};

uint32_t memRead(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    auto *c = static_cast<MemCtx *>(ctx);
    // 先减后比，避免 off + len 在 64 位上回绕
    if (off > c->data.size() || len > c->data.size() - off)
        return 0;
    std::memcpy(buf, c->data.data() + off, len);
    return 1;
}

uint32_t memWrite(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    auto *c = static_cast<MemCtx *>(ctx);
    if (off > c->data.size() || len > c->data.size() - off)
        return 0;
    std::memcpy(c->data.data() + off, buf, len);
    return 1;
}

void memDeinit(void *ctx) {
    delete static_cast<MemCtx *>(ctx);
}

dsc::VirtioBlk::BackendOps memBackend(const std::map<std::string, std::string> &opts, dsc::Context &,
                                      std::string &error) {
    int mb = 16;
    if (auto it = opts.find("size_mb"); it != opts.end()) {
        const std::string &s = it->second;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), mb);
        if (ec != std::errc{} || ptr != s.data() + s.size()) {
            error = std::format("virtio_blk 的 mem 后端 size_mb 不是整数: '{}'", s);
            return {};
        }
    }
    if (mb <= 0 || mb > 4096) {
        error = "virtio_blk 的 mem 后端 size_mb 必须在 1..4096 之间";
        return {};
    }

    auto *ctx = new MemCtx();
    ctx->data.assign((size_t) mb * 1024 * 1024, 0);

    dsc::VirtioBlk::BackendOps ops;
    ops.ctx = ctx;
    ops.read = &memRead;
    ops.write = &memWrite;
    ops.deinit = &memDeinit;
    ops.size = ctx->data.size();
    return ops;
}

} // namespace

DCS_REGISTER_VIRTIO_BLK_BACKEND("mem", &memBackend);
