// DMA：配置寄存器、按字搬运、完成标志
#include <gtest/gtest.h>
#include <cstring>

#include <dcs/Circuit.h>
#include <dcs/components/platform/DMA.h>
#include <dcs/components/memory/WishboneMemory.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    DMA *dma = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk = nullptr;
    Net *c_adr = nullptr, *c_dat_o = nullptr, *c_dat_i = nullptr, *c_we = nullptr;
    Net *c_sel = nullptr, *c_stb = nullptr, *c_cyc = nullptr, *c_ack = nullptr;
    Net *m_adr = nullptr, *m_dat_o = nullptr, *m_dat_i = nullptr, *m_we = nullptr;
    Net *m_sel = nullptr, *m_stb = nullptr, *m_cyc = nullptr, *m_ack = nullptr;

    Fixture() {
        auto d = std::make_unique<DMA>("dma0");
        dma = d.get();
        circuit->addComponent(std::move(d));
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

        circuit->connect(dma, "cfg_adr_i", c_adr);
        circuit->connect(dma, "cfg_dat_i", c_dat_o);
        circuit->connect(dma, "cfg_we_i", c_we);
        circuit->connect(dma, "cfg_sel_i", c_sel);
        circuit->connect(dma, "cfg_stb_i", c_stb);
        circuit->connect(dma, "cfg_cyc_i", c_cyc);
        circuit->connect(dma, "cfg_dat_o", c_dat_i);
        circuit->connect(dma, "cfg_ack_o", c_ack);
        circuit->connect(dma, "m_adr_o", m_adr);
        circuit->connect(dma, "m_dat_o", m_dat_o);
        circuit->connect(dma, "m_we_o", m_we);
        circuit->connect(dma, "m_sel_o", m_sel);
        circuit->connect(dma, "m_stb_o", m_stb);
        circuit->connect(dma, "m_cyc_o", m_cyc);
        circuit->connect(dma, "m_dat_i", m_dat_i);
        circuit->connect(dma, "m_ack_i", m_ack);

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
        circuit->setWire(clk->id(), (uint8_t) 0);
        circuit->setWire(c_stb->id(), (uint8_t) 0);
        circuit->setWire(c_cyc->id(), (uint8_t) 0);
        circuit->setWire(c_we->id(), (uint8_t) 0);
        circuit->setWire(c_sel->id(), (uint8_t) 0x0F);
        for (int i = 0; i < 2; ++i) cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), (uint8_t) 0);
        circuit->tick();
        circuit->setWire(clk->id(), (uint8_t) 1);
        circuit->tick();
    }
    void write(uint32_t addr, uint32_t v) {
        circuit->setWire(c_adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(c_dat_o->id(), {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)});
        circuit->setWire(c_we->id(), (uint8_t) 1);
        circuit->setWire(c_stb->id(), (uint8_t) 1);
        circuit->setWire(c_cyc->id(), (uint8_t) 1);
        for (int i = 0; i < 4; ++i) cycle();
        circuit->setWire(c_stb->id(), (uint8_t) 0);
        circuit->setWire(c_cyc->id(), (uint8_t) 0);
        for (int i = 0; i < 2; ++i) cycle();
    }
    uint32_t read(uint32_t addr) {
        circuit->setWire(c_adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(c_we->id(), (uint8_t) 0);
        circuit->setWire(c_stb->id(), (uint8_t) 1);
        circuit->setWire(c_cyc->id(), (uint8_t) 1);
        for (int i = 0; i < 6; ++i) cycle();
        auto v = circuit->getWire(c_dat_i->id());
        circuit->setWire(c_stb->id(), (uint8_t) 0);
        circuit->setWire(c_cyc->id(), (uint8_t) 0);
        for (int i = 0; i < 2; ++i) cycle();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    void put(uint32_t addr, uint32_t v) { std::memcpy(mem->writableMemPtr() + addr, &v, 4); }
    uint32_t peek(uint32_t addr) { uint32_t v; std::memcpy(&v, mem->writableMemPtr() + addr, 4); return v; }
};
} // namespace

TEST(DmaTest, RegistersRoundTrip) {
    Fixture f;
    f.compile();
    f.write(0x00, 0x1000);
    f.write(0x04, 0x2000);
    f.write(0x08, 0x40);
    EXPECT_EQ(f.read(0x00), 0x1000u);
    EXPECT_EQ(f.read(0x04), 0x2000u);
    EXPECT_EQ(f.read(0x08), 0x40u);
}

TEST(DmaTest, CopiesWords) {
    Fixture f;
    f.compile();
    f.put(0x100, 0x11223344);
    f.put(0x104, 0x55667788);
    f.put(0x200, 0);
    f.put(0x204, 0);
    f.write(0x00, 0x100); // SRC
    f.write(0x04, 0x200); // DST
    f.write(0x08, 8);     // 8 字节 = 2 个字
    f.write(0x0C, 1);     // 启动
    for (int i = 0; i < 80; ++i)
        f.cycle();
    EXPECT_EQ(f.peek(0x200), 0x11223344u) << "第一个字应被搬过去";
    EXPECT_EQ(f.peek(0x204), 0x55667788u) << "第二个字应被搬过去";
    EXPECT_EQ(f.read(0x10) & 2u, 2u) << "应置完成标志";
}

TEST(DmaTest, ZeroLengthDoesNotStart) {
    Fixture f;
    f.compile();
    f.write(0x00, 0x100);
    f.write(0x04, 0x200);
    f.write(0x08, 0);   // 长度 0
    f.write(0x0C, 1);   // 试图启动
    for (int i = 0; i < 10; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x10) & 1u, 0u) << "长度为 0 不能进入忙碌状态";
    EXPECT_EQ(f.read(0x10) & 2u, 0u) << "也不应报告完成";
}

TEST(DmaTest, LengthRoundedDownToWordMultiple) {
    Fixture f;
    f.compile();
    f.put(0x100, 0xAAAAAAAA);
    f.put(0x104, 0xBBBBBBBB);
    f.put(0x200, 0);
    f.put(0x204, 0);
    f.write(0x00, 0x100);
    f.write(0x04, 0x200);
    f.write(0x08, 6); // 6 字节向下取整成 4，只搬一个字
    f.write(0x0C, 1);
    for (int i = 0; i < 80; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x08), 4u) << "长度寄存器应按字取整";
    EXPECT_EQ(f.peek(0x200), 0xAAAAAAAAu);
    EXPECT_EQ(f.peek(0x204), 0u) << "不足一个字的部分不能搬";
}

TEST(DmaTest, BusyIgnoresRegisterWrites) {
    Fixture f;
    f.compile();
    for (int i = 0; i < 8; ++i)
        f.put(0x100 + i * 4, 0x1000u + (uint32_t) i);
    for (int i = 0; i < 8; ++i)
        f.put(0x200 + i * 4, 0);
    f.write(0x00, 0x100);
    f.write(0x04, 0x200);
    f.write(0x08, 32); // 8 个字，够长，贯穿下面几次寄存器访问
    f.write(0x0C, 1);
    f.cycle(); // 已经进入搬运状态
    EXPECT_EQ(f.read(0x10) & 1u, 1u) << "写之前必须还在搬运中，否则这个用例测不到保护逻辑";
    f.write(0x00, 0x9999); // 忙时改源地址
    EXPECT_NE(f.read(0x00), 0x9999u) << "搬运期间源地址不能被改掉";
    for (int i = 0; i < 300; ++i)
        f.cycle();
    EXPECT_EQ(f.peek(0x200), 0x1000u);
}

TEST(DmaTest, StatusBusyThenDone) {
    Fixture f;
    f.compile();
    // 用足够长的搬运，否则 read() 内部那几拍就搬完了，看不到忙碌态
    for (int i = 0; i < 8; ++i)
        f.put(0x100 + i * 4, 0x1000u + (uint32_t) i);
    for (int i = 0; i < 8; ++i)
        f.put(0x200 + i * 4, 0);
    f.write(0x00, 0x100);
    f.write(0x04, 0x200);
    f.write(0x08, 32); // 8 个字
    f.write(0x0C, 1);
    for (int i = 0; i < 2; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x10) & 1u, 1u) << "搬运中应置忙碌位";
    for (int i = 0; i < 200; ++i)
        f.cycle();
    uint32_t st = f.read(0x10);
    EXPECT_EQ(st & 1u, 0u) << "结束后应清忙碌位";
    EXPECT_EQ(st & 2u, 2u) << "结束后应置完成位";
}
