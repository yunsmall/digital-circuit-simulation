// WishboneDcache 单元测试：命中/未命中、写直通、监听失效、MMIO 旁路
#include <gtest/gtest.h>
#include <cstring>

#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/WishboneDcache.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    WishboneDcache *cache = nullptr;
    WishboneMemory *mem = nullptr;
    Net *c_adr = nullptr, *c_dat_o = nullptr, *c_dat_i = nullptr, *c_we = nullptr;
    Net *c_sel = nullptr, *c_stb = nullptr, *c_cyc = nullptr, *c_ack = nullptr;
    Net *m_adr = nullptr, *m_dat_o = nullptr, *m_dat_i = nullptr, *m_we = nullptr;
    Net *m_sel = nullptr, *m_stb = nullptr, *m_cyc = nullptr, *m_ack = nullptr;
    Net *sn_adr = nullptr, *sn_we = nullptr, *sn_stb = nullptr, *sn_cyc = nullptr;

    static constexpr uint32_t BASE = 0x80000000u;

    explicit Fixture(int lines = 16, uint32_t base = BASE, uint32_t size = 0x10000) {
        auto c = std::make_unique<WishboneDcache>("dcache0", lines, base, size);
        cache = c.get();
        circuit->addComponent(std::move(c));
        auto m = std::make_unique<WishboneMemory>("mem0", 16, 32); // 64 KB
        mem = m.get();
        circuit->addComponent(std::move(m));

        auto mk = [&](const char *n) { return circuit->createNet(n); };
        c_adr = mk("c_adr"); c_dat_o = mk("c_dat_o"); c_dat_i = mk("c_dat_i"); c_we = mk("c_we");
        c_sel = mk("c_sel"); c_stb = mk("c_stb"); c_cyc = mk("c_cyc"); c_ack = mk("c_ack");
        m_adr = mk("m_adr"); m_dat_o = mk("m_dat_o"); m_dat_i = mk("m_dat_i"); m_we = mk("m_we");
        m_sel = mk("m_sel"); m_stb = mk("m_stb"); m_cyc = mk("m_cyc"); m_ack = mk("m_ack");
        sn_adr = mk("sn_adr"); sn_we = mk("sn_we"); sn_stb = mk("sn_stb"); sn_cyc = mk("sn_cyc");

        circuit->connect(cache, "s_adr_i", c_adr);
        circuit->connect(cache, "s_dat_i", c_dat_o);
        circuit->connect(cache, "s_we_i", c_we);
        circuit->connect(cache, "s_sel_i", c_sel);
        circuit->connect(cache, "s_stb_i", c_stb);
        circuit->connect(cache, "s_cyc_i", c_cyc);
        circuit->connect(cache, "s_dat_o", c_dat_i);
        circuit->connect(cache, "s_ack_o", c_ack);

        circuit->connect(cache, "m_adr_o", m_adr);
        circuit->connect(cache, "m_dat_o", m_dat_o);
        circuit->connect(cache, "m_we_o", m_we);
        circuit->connect(cache, "m_sel_o", m_sel);
        circuit->connect(cache, "m_stb_o", m_stb);
        circuit->connect(cache, "m_cyc_o", m_cyc);
        circuit->connect(cache, "m_dat_i", m_dat_i);
        circuit->connect(cache, "m_ack_i", m_ack);

        circuit->connect(cache, "sn_adr_i", sn_adr);
        circuit->connect(cache, "sn_we_i", sn_we);
        circuit->connect(cache, "sn_stb_i", sn_stb);
        circuit->connect(cache, "sn_cyc_i", sn_cyc);

        circuit->connect(mem, "wb_adr_i", m_adr);
        circuit->connect(mem, "wb_dat_i", m_dat_o);
        circuit->connect(mem, "wb_we_i", m_we);
        circuit->connect(mem, "wb_sel_i", m_sel);
        circuit->connect(mem, "wb_stb_i", m_stb);
        circuit->connect(mem, "wb_cyc_i", m_cyc);
        circuit->connect(mem, "wb_dat_o", m_dat_i);
        circuit->connect(mem, "wb_ack_o", m_ack);
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        setWire(c_stb, 0);
        setWire(c_cyc, 0);
        setWire(c_we, 0);
        setWire(c_sel, 0x0F);
        setWire(sn_stb, 0);
        setWire(sn_cyc, 0);
        tick();
    }

    void setWire(Net *n, std::vector<uint8_t> v) {
        circuit->setWire(n->id(), std::move(v));
    }
    // 单个值的写法走这条：用花括号会被当成 vector 初值，参数类型对不上
    void setWire(Net *n, uint64_t v) {
        circuit->setWire(n->id(), v);
    }
    void tick() {
        circuit->tick();
    }
    void setAddr(Net *n, uint32_t a) {
        circuit->setWire(n->id(),
                         {(uint8_t) (a & 0xFF), (uint8_t) ((a >> 8) & 0xFF), (uint8_t) ((a >> 16) & 0xFF),
                          (uint8_t) ((a >> 24) & 0xFF)});
    }
    static uint32_t u32(const std::vector<uint8_t> &v) {
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    void put(uint32_t addr, uint32_t v) {
        std::memcpy(mem->writableMemPtr() + (addr & 0xFFFF), &v, 4);
    }
    uint32_t peek(uint32_t addr) const {
        uint32_t v = 0;
        std::memcpy(&v, mem->writableMemPtr() + (addr & 0xFFFF), 4);
        return v;
    }

    // 发起一笔事务，返回读数据和（可选）等到 ack 用了几个 tick
    uint32_t access(uint32_t addr, bool we, uint32_t wdata, uint8_t sel = 0x0F, int *ticks = nullptr,
                    int max_ticks = 100) {
        setWire(c_sel, sel);
        setAddr(c_adr, addr);
        setWire(c_dat_o, {(uint8_t) (wdata & 0xFF), (uint8_t) ((wdata >> 8) & 0xFF), (uint8_t) ((wdata >> 16) & 0xFF),
                          (uint8_t) ((wdata >> 24) & 0xFF)});
        setWire(c_we, (uint8_t) (we ? 1 : 0));
        setWire(c_stb, 1);
        setWire(c_cyc, 1);
        uint32_t result = 0;
        int i = 0;
        for (; i < max_ticks; ++i) {
            tick();
            if (circuit->getWire(c_ack->id())[0]) {
                result = u32(circuit->getWire(c_dat_i->id()));
                break;
            }
        }
        setWire(c_stb, 0);
        setWire(c_cyc, 0);
        tick();
        tick();
        if (ticks)
            *ticks = i + 1;
        return result;
    }

    // 模拟互连把其它主设备的写事务转发到 RAM 侧：stb 上升沿触发失效
    void snoopWrite(uint32_t addr, bool we = true) {
        setAddr(sn_adr, addr);
        setWire(sn_we, (uint8_t) (we ? 1 : 0));
        setWire(sn_stb, 1);
        setWire(sn_cyc, 1);
        tick();
        setWire(sn_stb, 0);
        setWire(sn_cyc, 0);
        tick();
    }
};
} // namespace

TEST(DcacheTest, ReadMissThenHitIsFast) {
    Fixture f;
    f.compile();
    f.put(Fixture::BASE + 0x100, 0xDEADBEEF);
    int miss_ticks = 0, hit_ticks = 0;
    EXPECT_EQ(f.access(Fixture::BASE + 0x100, false, 0, 0x0F, &miss_ticks), 0xDEADBEEFu);
    EXPECT_EQ(f.access(Fixture::BASE + 0x100, false, 0, 0x0F, &hit_ticks), 0xDEADBEEFu);
    EXPECT_GT(miss_ticks, 1) << "未命中要走总线";
    EXPECT_EQ(hit_ticks, 1) << "命中读必须在组合阶段当拍应答";
}

TEST(DcacheTest, LineIsSectoredPerWord) {
    Fixture f;
    f.compile();
    f.put(Fixture::BASE + 0x200, 0x11111111);
    f.put(Fixture::BASE + 0x204, 0x22222222);
    EXPECT_EQ(f.access(Fixture::BASE + 0x200, false, 0), 0x11111111u);
    // 同一行内另一个字：本缓存按字取，不是整行填充，第一次访问仍然未命中
    int ticks = 0;
    EXPECT_EQ(f.access(Fixture::BASE + 0x204, false, 0, 0x0F, &ticks), 0x22222222u);
    EXPECT_GT(ticks, 1);
    // 取过之后两个字都应该命中
    EXPECT_EQ(f.access(Fixture::BASE + 0x200, false, 0, 0x0F, &ticks), 0x11111111u);
    EXPECT_EQ(ticks, 1);
    EXPECT_EQ(f.access(Fixture::BASE + 0x204, false, 0, 0x0F, &ticks), 0x22222222u);
    EXPECT_EQ(ticks, 1);
}

TEST(DcacheTest, WriteThroughReachesMemoryAndKeepsLine) {
    Fixture f;
    f.compile();
    f.put(Fixture::BASE + 0x300, 0x11223344);
    f.access(Fixture::BASE + 0x300, false, 0); // 填充
    int ticks = 0;
    EXPECT_EQ(f.access(Fixture::BASE + 0x300, true, 0xCAFEBABE, 0x0F, &ticks), 0);
    EXPECT_GT(ticks, 1) << "写直通要等存储器应答";
    EXPECT_EQ(f.peek(Fixture::BASE + 0x300), 0xCAFEBABEu);
    EXPECT_EQ(f.access(Fixture::BASE + 0x300, false, 0, 0x0F, &ticks), 0xCAFEBABEu);
    EXPECT_EQ(ticks, 1) << "命中行要跟着更新，写后读不应再走总线";
}

TEST(DcacheTest, ByteWriteOnlyTouchesSelectedByte) {
    Fixture f;
    f.compile();
    f.put(Fixture::BASE + 0x400, 0x11223344);
    f.access(Fixture::BASE + 0x400, false, 0);
    f.access(Fixture::BASE + 0x400, true, 0xAA, 0x01); // 只写最低字节
    EXPECT_EQ(f.peek(Fixture::BASE + 0x400), 0x112233AAu);
    EXPECT_EQ(f.access(Fixture::BASE + 0x400, false, 0), 0x112233AAu);
}

TEST(DcacheTest, WriteMissDoesNotValidatePartialWord) {
    Fixture f;
    f.compile();
    f.put(Fixture::BASE + 0x500, 0x55667788);
    // 没读过这一行，直接按字节写：行里其它字节是旧的，不能把整个字标成有效
    f.access(Fixture::BASE + 0x500, true, 0x99, 0x01);
    EXPECT_EQ(f.peek(Fixture::BASE + 0x500), 0x55667799u);
    EXPECT_EQ(f.access(Fixture::BASE + 0x500, false, 0), 0x55667799u);
}

TEST(DcacheTest, MmioBypassesCache) {
    Fixture f;
    f.compile();
    const uint32_t mmio = 0x1000; // 段外
    f.put(mmio, 0xAAAA0001);
    EXPECT_EQ(f.access(mmio, false, 0), 0xAAAA0001u);
    f.put(mmio, 0xBBBB0002); // 外部改内存：旁路访问必须读到新值
    EXPECT_EQ(f.access(mmio, false, 0), 0xBBBB0002u);
    f.access(mmio, true, 0xCCCC0003);
    EXPECT_EQ(f.peek(mmio), 0xCCCC0003u);
    EXPECT_EQ(f.access(mmio, false, 0), 0xCCCC0003u);
}

TEST(DcacheTest, SnoopWriteInvalidatesLine) {
    Fixture f;
    f.compile();
    const uint32_t a = Fixture::BASE + 0x600;
    f.put(a, 0x01020304);
    f.access(a, false, 0); // 填充
    f.put(a, 0x0A0B0C0D);  // 外部（DMA）改内存
    f.snoopWrite(a);       // 互连把这个写转发到 RAM 侧
    EXPECT_EQ(f.access(a, false, 0), 0x0A0B0C0Du) << "监听失效后必须重新取数";
}

TEST(DcacheTest, SnoopOtherLineKeepsData) {
    Fixture f;
    f.compile();
    const uint32_t a = Fixture::BASE + 0x700;
    const uint32_t b = Fixture::BASE + 0x900; // 不同行
    f.put(a, 0x11112222);
    f.access(a, false, 0);
    f.put(a, 0x33334444);
    f.snoopWrite(b);
    EXPECT_EQ(f.access(a, false, 0), 0x11112222u) << "别的行被写不该影响本行";
}

TEST(DcacheTest, SnoopReadDoesNotInvalidate) {
    Fixture f;
    f.compile();
    const uint32_t a = Fixture::BASE + 0xA00;
    f.put(a, 0x55556666);
    f.access(a, false, 0);
    f.put(a, 0x77778888);
    f.snoopWrite(a, false); // we=0：读事务
    EXPECT_EQ(f.access(a, false, 0), 0x55556666u);
}

TEST(DcacheTest, OwnWriteThroughIsNotInvalidated) {
    Fixture f;
    f.compile();
    const uint32_t a = Fixture::BASE + 0xB00;
    f.put(a, 0);
    f.access(a, false, 0); // 填充

    // 写直通期间，互连会把这次写也转发到 RAM 侧；它不能把刚更新的行失效掉
    f.setWire(f.c_sel, 0x0F);
    f.setAddr(f.c_adr, a);
    f.setWire(f.c_dat_o, {0xEF, 0xBE, 0xAD, 0xDE});
    f.setWire(f.c_we, 1);
    f.setWire(f.c_stb, 1);
    f.setWire(f.c_cyc, 1);
    bool ack = false;
    for (int i = 0; i < 20 && !ack; ++i) {
        f.setAddr(f.sn_adr, a);
        f.setWire(f.sn_we, 1);
        f.setWire(f.sn_stb, 1);
        f.setWire(f.sn_cyc, 1);
        f.tick();
        ack = f.circuit->getWire(f.c_ack->id())[0];
    }
    f.setWire(f.sn_stb, 0);
    f.setWire(f.sn_cyc, 0);
    f.setWire(f.c_stb, 0);
    f.setWire(f.c_cyc, 0);
    f.tick();
    f.tick();

    EXPECT_TRUE(ack);
    int ticks = 0;
    EXPECT_EQ(f.access(a, false, 0, 0x0F, &ticks), 0xDEADBEEFu);
    EXPECT_EQ(ticks, 1) << "自己的写直通不算外部修改，行应保持有效";
}

TEST(DcacheTest, OwnWriteEchoOnCompletionTickIsIgnored) {
    Fixture f;
    f.compile();
    const uint32_t a = Fixture::BASE + 0xE00;
    f.put(a, 0);
    f.access(a, false, 0); // 填充

    // 真实 SoC 里互连会把这次写直通转发到 RAM 侧，回波上升沿正好落在完成那一拍：
    // 那时 busy 已经清掉，靠写事务期间记住的地址才能认出是自己的回波
    f.setWire(f.c_sel, 0x0F);
    f.setAddr(f.c_adr, a);
    f.setWire(f.c_dat_o, {0x11, 0x22, 0x33, 0x44});
    f.setWire(f.c_we, 1);
    f.setWire(f.c_stb, 1);
    f.setWire(f.c_cyc, 1);
    f.tick(); // 第 1 拍：请求上总线
    f.setAddr(f.sn_adr, a);
    f.setWire(f.sn_we, 1);
    f.setWire(f.sn_stb, 1);
    f.setWire(f.sn_cyc, 1);
    f.tick(); // 第 2 拍：完成 + 回波上升沿
    f.setWire(f.sn_stb, 0);
    f.setWire(f.sn_cyc, 0);
    f.setWire(f.c_stb, 0);
    f.setWire(f.c_cyc, 0);
    f.tick();
    f.tick();

    int ticks = 0;
    EXPECT_EQ(f.access(a, false, 0, 0x0F, &ticks), 0x44332211u);
    EXPECT_EQ(ticks, 1) << "自己写直通的回波不能把刚更新的行失效";
}

TEST(DcacheTest, DirectMappedConflictEvicts) {
    Fixture f(4); // 4 行：0x100 和 0x140 同索引
    f.compile();
    const uint32_t a = Fixture::BASE + 0x100;
    const uint32_t b = Fixture::BASE + 0x140;
    f.put(a, 0xAAAA1111);
    f.put(b, 0xBBBB2222);
    EXPECT_EQ(f.access(a, false, 0), 0xAAAA1111u);
    EXPECT_EQ(f.access(b, false, 0), 0xBBBB2222u);
    int ticks = 0;
    EXPECT_EQ(f.access(a, false, 0, 0x0F, &ticks), 0xAAAA1111u);
    EXPECT_GT(ticks, 1) << "同索引的行被挤掉，必须重新填充";
}

TEST(DcacheTest, TagIsNotTruncated) {
    Fixture f(4);
    f.compile();
    // 同一行索引、tag 不同：不能被当成命中。0x40 的位 4-5 与 0 相同，
    // 行索引一样，但 bit6 以上不同，落进不同 tag
    const uint32_t a = Fixture::BASE + 0x0000;
    const uint32_t b = Fixture::BASE + 0x0040;
    f.put(a, 0x12345678);
    f.put(b, 0x87654321);
    EXPECT_EQ(f.access(a, false, 0), 0x12345678u);
    EXPECT_EQ(f.access(b, false, 0), 0x87654321u);
}

TEST(DcacheTest, ResetDropsAllLines) {
    Fixture f;
    f.compile();
    const uint32_t a = Fixture::BASE + 0xC00;
    f.put(a, 0x0BADF00D);
    f.access(a, false, 0);
    f.circuit->reset();
    f.setWire(f.c_stb, 0);
    f.setWire(f.c_cyc, 0);
    int ticks = 0;
    EXPECT_EQ(f.access(a, false, 0, 0x0F, &ticks), 0x0BADF00Du);
    EXPECT_GT(ticks, 1) << "复位后缓存必须清空";
}

TEST(DcacheTest, BadLineCountRejected) {
    EXPECT_THROW(WishboneDcache("c", 3), std::invalid_argument);
    EXPECT_THROW(WishboneDcache("c", 0), std::invalid_argument);
    EXPECT_THROW(WishboneDcache("c", 16384), std::invalid_argument);
}

TEST(DcacheTest, BackToBackStoreLoadSeesNewValue) {
    Fixture f;
    f.compile();
    const uint32_t a = Fixture::BASE + 0xD00;
    f.put(a, 0);
    for (uint32_t i = 0; i < 8; ++i) {
        f.access(a, true, 0x1000 + i);
        EXPECT_EQ(f.access(a, false, 0), 0x1000 + i) << "写后立刻读必须读到新值, i=" << i;
    }
    EXPECT_EQ(f.peek(a), 0x1007u);
}

TEST(DcacheTest, JsonRoundTripKeepsParams) {
    Fixture f(8, Fixture::BASE, 0x10000);
    f.compile();
    std::string json = f.circuit->exportJson();
    std::string err;
    auto c2 = Circuit::fromJson(json, err);
    ASSERT_TRUE(c2 != nullptr) << err;
    bool found = false;
    for (auto &c: c2->components())
        if (c->typeName() == "wb_dcache") {
            found = true;
            EXPECT_EQ(c->exportParams().at("lines"), "8");
            EXPECT_EQ(c->exportParams().at("base"), std::to_string(Fixture::BASE));
            EXPECT_EQ(c->exportParams().at("size"), "65536");
        }
    EXPECT_TRUE(found);
}

// base/size 超过 32 位时不能静默截断：截断会把缓存段挪到别的地址
TEST(DcacheTest, RejectsParamsAbove32Bits) {
    auto &f = dsc::ComponentFactory::instance();
    std::string err;
    auto c = f.create("wb_dcache", "dc", {{"base", "4294967296"}}, err);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(err.find("32 位"), std::string::npos) << err;

    err.clear();
    auto c2 = f.create("wb_dcache", "dc2", {{"size", "4294967296"}}, err);
    EXPECT_EQ(c2, nullptr);
    EXPECT_NE(err.find("32 位"), std::string::npos) << err;
}
