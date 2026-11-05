// virtio-blk：寄存器、队列握手、镜像读写
#include "common.h"

#include <dcs/Context.h>
#include <dcs/components/platform/VirtioBlk.h>

using namespace dsc;
using namespace dsc::test;

TEST(VirtioTest, MagicVersionAndDeviceId) {
    VirtioFixture<VirtioBlk> f;
    EXPECT_EQ(f.read(0x000), 0x74726976u) << "magic 应为 \"virt\"";
    EXPECT_EQ(f.read(0x004), 2u) << "只实现 modern（version 2）";
    EXPECT_EQ(f.read(0x008), 2u) << "blk 的 device id 是 2";
    EXPECT_EQ(f.read(0x00C), 0x444353u);
}

TEST(VirtioTest, FeatureSelectors) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x014, 0); // sel 0
    EXPECT_EQ(f.read(0x010), 0u) << "没有额外特性";
    f.write(0x014, 1); // sel 1
    EXPECT_EQ(f.read(0x010), 1u) << "sel1 必须声明 VIRTIO_F_VERSION_1";
}

// QueueNumMax 太小会让 Linux 的 virtio-net 判定环永远不够用：
// num_free < 2 + MAX_SKB_FRAGS 时驱动直接停掉 TX 队列，发包后不再 notify
TEST(VirtioTest, QueueNumMaxIsUsable) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x030, 0); // queue selector
    EXPECT_GE(f.read(0x034), 64u);
}

TEST(VirtioTest, ReadFromBackendImage) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    for (int i = 0; i < 512; i++)
        f.image[i] = (uint8_t) i;

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0); // notify
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "请求完成后 used.idx 应加一";
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[data + i], (uint8_t) i) << "第 " << i << " 字节不匹配";
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 1) << "完成请求后应拉中断";
}

// deinit 会释放后端并把 ctx 置空；重新编译必须重新打开介质，
// 否则 JIT 里的 dcs_virtio_io 会拿着空 ctx 调后端
TEST(VirtioTest, RecompileAfterDeinitReopensBackend) {
    VirtioFixture<VirtioBlk> f; // 默认 mem 后端
    f.c->deinit();
    auto err = f.c->compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    f.c->init();

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "请求完成后 used.idx 应加一";
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
}

// 非对齐 buffer：主端口必须按字内剩余空间拆事务，SEL 不能跨出 4 位
TEST(VirtioTest, UnalignedDataBuffer) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    for (int i = 0; i < 512; i++)
        f.image[i] = (uint8_t) (0x40 + i);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x241, status = 0x300;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 4000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1);
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[data + i], (uint8_t) (0x40 + i)) << "第 " << i << " 字节不匹配";
}

TEST(VirtioTest, WriteToReadOnlyImageReturnsRoError) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadOnly);
    EXPECT_NE(f.read(0x010) & (1u << 5), 0u) << "只读后端应声明 VIRTIO_BLK_F_RO";

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, false);
    std::memset(f.mem() + data, 0xAA, 16);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 3) << "对只读后端写入应返回 RO 错误";
    EXPECT_EQ(f.image[0], 0) << "镜像不能被改动";
}

TEST(VirtioTest, UnsupportedRequestReturnsUnsupp) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 99, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.mem()[status], 2) << "未知请求类型应返回 UNSUPP";
}

TEST(VirtioTest, InterruptAckClearsLine) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.setupBlkQueue(desc, avail, used, 0, 0x200, 0x240, 0x280, 16, 0, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.read(0x060) & 1u, 1u) << "interrupt_status 应置 VRING 位";
    f.write(0x064, 1); // ack
    for (int i = 0; i < 2; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 0) << "ack 后中断线应撤销";
}

TEST(VirtioTest, StatusZeroResetsQueue) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x030, 0);
    f.write(0x038, 4);
    f.write(0x044, 1);
    f.write(0x070, 0);
    f.write(0x030, 0);
    EXPECT_EQ(f.read(0x044), 0u) << "status 写 0 应复位队列";
}

// QueueNum 会被当成 avail/used 环的除数：驱动写 0 时不能出现 % 0
TEST(VirtioTest, ZeroQueueNumDoesNotDivideByZero) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.setupBlkQueue(desc, avail, used, 0, 0x200, 0x240, 0x280, 16, 0, 0, true);
    f.write(0x030, 0);
    f.write(0x038, 0); // 0 会被钳到 1
    f.write(0x080, desc); f.write(0x084, 0);
    f.write(0x090, avail); f.write(0x094, 0);
    f.write(0x0A0, used); f.write(0x0A4, 0);
    f.write(0x044, 1);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++)
        f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "qnum=0 时仍要能正常处理（钳到 1）";
    EXPECT_EQ(f.mem()[0x280], 0) << "请求应成功完成";
}

// 驱动在设备处理中途重复 notify 不能把当前阶段拽回开头并丢掉请求
TEST(VirtioTest, NotifyDuringProcessingDoesNotLoseRequest) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.setupBlkQueue(desc, avail, used, 0, 0x200, 0x240, 0x280, 64, 0, 0, true);
    f.configureQueue(desc, avail, used);

    for (int i = 0; i < 300; i++) {
        f.write(0x050, 0); // 连续发包时驱动的常规行为
        if (f.get16(used + 2) == 1)
            break;
    }
    EXPECT_EQ(f.get16(used + 2), 1u) << "处理期间收到 notify 后请求没有完成";
    EXPECT_EQ(f.mem()[0x280], 0);
}

// status=0 复位时在途的主端口事务必须停止，不能继续往 guest 内存里搬数据
TEST(VirtioTest, StatusZeroAbortsInflightMaster) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    for (int i = 0; i < 512; i++)
        f.image[i] = (uint8_t) (0x30 + i);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.setupBlkQueue(desc, avail, used, 0, 0x200, 0x400, 0x700, 512, 0, 0, true);
    std::memset(f.mem() + 0x400, 0xEE, 512);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 60; i++)
        f.tick(); // 已经进入主端口搬运阶段

    f.write(0x070, 0);
    for (int i = 0; i < 3000; i++)
        f.tick();

    int dirty = 0;
    for (int i = 256; i < 512; i++)
        if (f.mem()[0x400 + i] != 0xEE)
            dirty++;
    EXPECT_EQ(dirty, 0) << "复位后在途事务还在写 guest 内存";
    EXPECT_EQ(f.get16(used + 2), 0u);
}

namespace {
struct OffsetRecorder {
    uint64_t off = 0;
};
uint32_t recordRead(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    static_cast<OffsetRecorder *>(ctx)->off = off;
    std::memset(buf, 0x5A, len);
    return 1;
}
} // namespace

// 规范 2.6.4：sector 是 64 位。偏移超过 4GiB 时必须把高 32 位传给后端，
// 容量寄存器的两个 32 位半也要分开返回
TEST(VirtioTest, LargeSectorUsesSixtyFourBitOffset) {
    OffsetRecorder rec;
    VirtioBlk::BackendOps ops;
    ops.ctx = &rec;
    ops.read = &recordRead;
    // 容量 2^32+5 个扇区：高半非 0，能同时验证 0x100/0x104 两个寄存器
    ops.size = 512ull * ((1ull << 32) + 5);
    VirtioFixture<VirtioBlk> f(BlkImage::None, &ops);

    EXPECT_EQ(f.read(0x100), 5u) << "capacity 低 32 位";
    EXPECT_EQ(f.read(0x104), 1u) << "capacity 高 32 位";

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint64_t sector = 0x800001; // 字节偏移 0x100000200，32 位装不下
    f.setupBlkQueue(desc, avail, used, 0, 0x200, 0x240, 0x280, 16, 0, sector, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++)
        f.tick();

    EXPECT_EQ(rec.off, sector * 512) << "后端拿到的偏移被截成 32 位";
    EXPECT_EQ(f.mem()[0x280], 0);
    EXPECT_EQ(f.get16(used + 2), 1);
}

// 请求头描述符不足 16 字节：不能照 16 字节越界读，直接完成请求
TEST(VirtioTest, ShortHeaderDescriptorCompletes) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.putDesc(desc, 0, 0x200, 8, 1, 1);       // 头只有 8 字节
    f.putDesc(desc, 1, 0x240, 16, 3, 2);      // 数据
    f.putDesc(desc, 2, 0x280, 1, 2, 0);       // 状态
    f.put16(avail + 0, 0);
    f.put16(avail + 2, 1);
    f.put16(avail + 4, 0);
    f.put16(used + 0, 0);
    f.put16(used + 2, 0);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++)
        f.tick();

    EXPECT_EQ(f.get16(used + 2), 1u) << "畸形请求也必须归还描述符";
    EXPECT_EQ(f.usedLen(used, 0), 0u);
}

// 请求头没有 NEXT：状态字节没有合法的落点，不能写回请求头所在的内存
TEST(VirtioTest, HeaderWithoutNextDoesNotWriteHeader) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.putDesc(desc, 0, 0x200, 16, 0, 0);
    f.put16(avail + 0, 0);
    f.put16(avail + 2, 1);
    f.put16(avail + 4, 0);
    f.put16(used + 0, 0);
    f.put16(used + 2, 0);
    f.put32(0x200, 99); // 未知请求类型，保留下来当哨兵
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++)
        f.tick();

    EXPECT_EQ(f.get16(used + 2), 1u);
    EXPECT_EQ(f.mem()[0x200], 99) << "状态字节被写进了请求头";
}

// 零长度数据段没有字节可搬；必须跳到下一个描述符，不能停在数据阶段等一个
// 永远不会启动的主端口事务
TEST(VirtioTest, ZeroLengthDataSegmentDoesNotHang) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 0 /*data_len*/, 0 /*IN*/, 0, false);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++)
        f.tick();

    EXPECT_EQ(f.get16(used + 2), 1u) << "零长度数据段不能把请求卡死";
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
}

// 外部注入的后端优先：不能因为漏填 read 就被注册表里的同名后端静默覆盖，
// 这种不完整的后端要在 prepare 时明确报错
TEST(VirtioTest, ExternalBackendMissingReadIsRejected) {
    VirtioBlk dev("vd0");
    VirtioBlk::BackendOps empty;   // read/write 都没填
    dev.setBackend(empty);
    std::string err;
    EXPECT_FALSE(dev.prepare(Context::global(), err));
    EXPECT_NE(err.find("read"), std::string::npos) << err;
}

// init 当重置重复调用时，后端 init 只能执行一次（后端 init 不可重入）
TEST(VirtioBlk, RepeatedInitCallsBackendOnce) {
    struct Counters {
        int inits = 0;
        int deinits = 0;
    } ctr;
    dsc::Circuit c;
    auto dev = std::make_unique<VirtioBlk>("vb0");
    auto *vp = dev.get();
    c.addComponent(std::move(dev));
    VirtioBlk::BackendOps ops;
    ops.ctx = &ctr;
    ops.init = [](void *p) { ++static_cast<Counters *>(p)->inits; };
    ops.deinit = [](void *p) { ++static_cast<Counters *>(p)->deinits; };
    ops.read = [](void *, uint64_t, uint8_t *buf, uint32_t len) {
        std::memset(buf, 0, len);
        return 1u;
    };
    ops.size = 512;
    vp->setBackend(ops);
    ASSERT_EQ(c.compile().code, ErrorCode::OK);
    c.init();
    EXPECT_EQ(ctr.inits, 1);
    c.init();
    EXPECT_EQ(ctr.inits, 1) << "第二次 init 不能再调后端 init";
    c.deinit();
    EXPECT_EQ(ctr.deinits, 1);
}

// 描述符 addr 是 64 位、主端口只有 32 位：高 32 位非零时必须按错误完成，
// 不能截断成低 32 位去访问无关内存
TEST(VirtioTest, DescriptorAddressAbove32BitsIsRejected) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, true);
    f.mem()[data] = 0x5A;
    // 把 desc0（请求头）的地址高 32 位改成 1 → addr = 0x1_0000_0200
    const uint32_t hi = 1;
    std::memcpy(f.mem() + desc + 4, &hi, 4);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "非法请求也要回收（used.idx 前进）";
    EXPECT_EQ(f.usedLen(used, 0), 0u) << "越界地址不能产生任何数据搬运";
    EXPECT_EQ(f.mem()[data], 0x5Au) << "不能按截断后的地址去写内存";
}

// avail.ring 里的描述符索引越界时，不能拿 idx*16 去算描述符地址（会回绕）
TEST(VirtioTest, DescriptorIndexOutOfRangeIsRejected) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.put16(avail + 4, 4); // 队列长度 4，索引 4 越界
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "越界索引也要完成请求，不能让驱动永久等待";
    EXPECT_EQ(f.usedLen(used, 0), 0u);
}

// dnext 来自 guest：越界时必须按零字节完成，不能把描述符表外的内存
// 当成描述符继续解析（否则会对任意 guest 地址发起事务）
TEST(VirtioTest, OutOfRangeNextCompletesRequest) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, false);
    f.configureQueue(desc, avail, used, 4);
    f.putDesc(desc, 0, hdr, 16, 1, 100); // next >= qnum
    f.write(0x050, 0);                   // notify
    for (int i = 0; i < 200; i++) f.tick();
    EXPECT_EQ(f.get16(used + 2), 1) << "畸形链必须被回收而不是卡死在数据阶段";
}

// 链尾状态描述符声明 dlen=0 时没有 1 字节可写，不能写到描述符范围之外
TEST(VirtioTest, ZeroLengthStatusDescriptorIsNotWritten) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, false);
    f.configureQueue(desc, avail, used, 4);
    f.mem()[status] = 0xAA;
    f.putDesc(desc, 2, status, 0, 0, 0); // 状态描述符 dlen=0、无 NEXT
    f.write(0x050, 0);
    for (int i = 0; i < 200; i++) f.tick();
    EXPECT_EQ(f.get16(used + 2), 1);
    EXPECT_EQ(f.mem()[status], 0xAA) << "dlen=0 时不能写状态字节";
}
