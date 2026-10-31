// virtio 元件的内部常量：传输层寄存器偏移、队列阶段、blk 协议常量。
// 只有 src/components/platform 下的 virtio 实现文件包含
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>

#include "dcs/components/platform/VirtioMmio.h"

namespace dsc::virtio {

enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_M_DAT, I_M_ACK };
enum OutIdx { O_DAT = 0, O_ACK, O_IRQ, O_M_ADR, O_M_DAT, O_M_WE, O_M_SEL, O_M_STB, O_M_CYC };

// modern virtio-mmio 寄存器偏移（version 2）
constexpr uint32_t R_MAGIC = 0x000;
constexpr uint32_t R_VERSION = 0x004;
constexpr uint32_t R_DEVICE_ID = 0x008;
constexpr uint32_t R_VENDOR_ID = 0x00C;
constexpr uint32_t R_DEVICE_FEATURES = 0x010;
constexpr uint32_t R_DEVICE_FEATURES_SEL = 0x014;
constexpr uint32_t R_DRIVER_FEATURES = 0x020;
constexpr uint32_t R_DRIVER_FEATURES_SEL = 0x024;
constexpr uint32_t R_QUEUE_SEL = 0x030;
constexpr uint32_t R_QUEUE_NUM_MAX = 0x034;
constexpr uint32_t R_QUEUE_NUM = 0x038;
constexpr uint32_t R_QUEUE_READY = 0x044;
constexpr uint32_t R_QUEUE_NOTIFY = 0x050;
constexpr uint32_t R_INTERRUPT_STATUS = 0x060;
constexpr uint32_t R_INTERRUPT_ACK = 0x064;
constexpr uint32_t R_STATUS = 0x070;
constexpr uint32_t R_DESC_LOW = 0x080, R_DESC_HIGH = 0x084;
constexpr uint32_t R_AVAIL_LOW = 0x090, R_AVAIL_HIGH = 0x094;
constexpr uint32_t R_USED_LOW = 0x0A0, R_USED_HIGH = 0x0A4;
constexpr uint32_t R_CONFIG_GEN = 0x0FC;
constexpr uint32_t R_CONFIG = 0x100;

constexpr uint32_t VIRTIO_MMIO_MAGIC = 0x74726976; // "virt"
constexpr uint32_t VIRTIO_VENDOR = 0x444353;       // "DCS"
constexpr uint32_t QUEUE_MAX = 8;

// 队列状态机阶段（生成代码里用字面量）
constexpr int Q_IDLE = 0, Q_AVAIL_IDX = 1, Q_AVAIL_RING = 2, Q_DESC = 3, Q_HDR = 4, Q_DATA = 5,
              Q_STATUS = 6, Q_USED = 7, Q_USED_IDX = 8;

// blk 请求类型与状态字节
constexpr uint32_t BLK_T_IN = 0, BLK_T_OUT = 1, BLK_T_FLUSH = 4;
constexpr uint8_t BLK_S_OK = 0, BLK_S_IOERR = 1, BLK_S_UNSUPP = 2, BLK_S_RO = 3;

} // namespace dsc::virtio

namespace dsc {

struct VirtioMmio::State {
    BackendOps ops{};
    std::FILE *fp = nullptr;
    const uint8_t *image = nullptr;
    uint64_t image_size = 0;
    bool read_only = false;

    // virtio-input：宿主 push 的 evdev 事件环。dcs 是单线程的，宿主只在两次
    // tick 之间 push，JIT 直接读 head/tail，不用跨 JIT 回调
    static constexpr uint32_t EVT_RING = 64;
    InputEvent events[EVT_RING] = {};
    uint32_t evt_head = 0; // 宿主写
    uint32_t evt_tail = 0; // 设备读
};

} // namespace dsc
