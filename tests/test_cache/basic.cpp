// L1 数据缓存：命中/未命中、行填充、写直通
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/WishboneCache.h>
#include <dcs/components/WishboneMemory.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    WishboneCache *cache = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk = nullptr;
    Net *c_adr = nullptr, *c_dat_o = nullptr, *c_dat_i = nullptr, *c_we = nullptr;
    Net *c_sel = nullptr, *c_stb = nullptr, *c_cyc = nullptr, *c_ack = nullptr;
    Net *m_adr = nullptr, *m_dat_o = nullptr, *m_dat_i = nullptr, *m_we = nullptr;
    Net *m_sel = nullptr, *m_stb = nullptr, *m_cyc = nullptr, *m_ack = nullptr;

    Fixture() {
        auto c = std::make_unique<WishboneCache>("cache0", 16);
        cache = c.get();
        circuit->addComponent(std::move(c));
        auto m = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = m.get();
        circuit->addComponent(std::move(m));
        clk = circuit->createNet("clk");
        c_adr = circuit->createNet("c_adr"); c_dat_o = circuit->createNet("c_dat_o");
        c_dat_i = circuit->createNet("c_dat_i"); c_we = circuit->createNet("c_we");
        c_sel = circuit->createNet("c_sel"); c_stb = circuit->createNet("c_stb");
        c_cyc = circuit->createNet("c_cyc"); c_ack = circuit->createNet("c_ack");
        m_adr = circuit->createNet("m_adr"); m_dat_o = circuit->createNet("m_dat_o");
        m_dat_i = circuit->createNet("m_dat_i"); m_we = circuit->createNet("m_we");
        m_sel = circuit->createNet("m_sel"); m_stb = circuit->createNet("m_stb");
        m_cyc = circuit->createNet("m_cyc"); m_ack = circuit->createNet("m_ack");

        circuit->connect(cache, "wb_adr_i", c_adr);
        circuit->connect(cache, "wb_dat_i", c_dat_o);
        circuit->connect(cache, "wb_we_i", c_we);
        circuit->connect(cache, "wb_sel_i", c_sel);
        circuit->connect(cache, "wb_stb_i", c_stb);
        circuit->connect(cache, "wb_cyc_i", c_cyc);
        circuit->connect(cache, "wb_dat_o", c_dat_i);
        circuit->connect(cache, "wb_ack_o", c_ack);
        circuit->connect(cache, "wb_adr_o", m_adr);
        circuit->connect(cache, "wb_dat_i_o", m_dat_o);
        circuit->connect(cache, "wb_we_o", m_we);
        circuit->connect(cache, "wb_sel_o", m_sel);
        circuit->connect(cache, "wb_stb_o", m_stb);
        circuit->connect(cache, "wb_cyc_o", m_cyc);
        circuit->connect(cache, "wb_dat_o_i", m_dat_i);
        circuit->connect(cache, "wb_ack_o_i", m_ack);

        circuit->connect(mem, "clk", clk);
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
        circuit->setWire(clk->id(), {0});
        circuit->setWire(c_stb->id(), {0});
        circuit->setWire(c_cyc->id(), {0});
        circuit->setWire(c_we->id(), {0});
        circuit->setWire(c_sel->id(), {0x0F});
        for (int i = 0; i < 2; ++i) cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), {0});
        circuit->tick();
        circuit->setWire(clk->id(), {1});
        circuit->tick();
    }
    void put(uint32_t addr, uint32_t v) { std::memcpy(mem->writableMemPtr() + addr, &v, 4); }
    uint32_t peek(uint32_t addr) { uint32_t v; std::memcpy(&v, mem->writableMemPtr() + addr, 4); return v; }

    uint32_t access(uint32_t addr, bool we, uint32_t wdata, int max_cycles = 60) {
        circuit->setWire(c_adr->id(), {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF), (uint8_t)((addr >> 24) & 0xFF)});
        circuit->setWire(c_dat_o->id(), {(uint8_t)(wdata & 0xFF), (uint8_t)((wdata >> 8) & 0xFF), (uint8_t)((wdata >> 16) & 0xFF), (uint8_t)((wdata >> 24) & 0xFF)});
        circuit->setWire(c_we->id(), {(uint8_t)(we ? 1 : 0)});
        circuit->setWire(c_stb->id(), {1});
        circuit->setWire(c_cyc->id(), {1});
        uint32_t result = 0;
        for (int i = 0; i < max_cycles; ++i) {
            cycle();
            if (circuit->getWire(c_ack->id())[0]) {
                auto v = circuit->getWire(c_dat_i->id());
                result = uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
                break;
            }
        }
        circuit->setWire(c_stb->id(), {0});
        circuit->setWire(c_cyc->id(), {0});
        for (int i = 0; i < 2; ++i) cycle();
        return result;
    }
};
} // namespace

TEST(CacheTest, ReadMissThenHit) {
    Fixture f;
    f.compile();
    f.put(0x100, 0xDEADBEEF);
    f.put(0x104, 0x12345678);
    EXPECT_EQ(f.access(0x100, false, 0), 0xDEADBEEFu) << "首次读未命中，应从存储器取回";
    uint64_t m1 = f.cache->misses();
    EXPECT_EQ(f.access(0x104, false, 0), 0x12345678u) << "同一行内第二次读应命中";
    EXPECT_EQ(f.cache->misses(), m1) << "命中不应增加未命中计数";
}

TEST(CacheTest, WriteThroughReachesMemory) {
    Fixture f;
    f.compile();
    f.put(0x200, 0);
    f.access(0x200, true, 0xCAFEBABE);
    EXPECT_EQ(f.peek(0x200), 0xCAFEBABEu) << "写直通应该落到存储器";
}
