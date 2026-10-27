// WishboneROMAdapter 测试: ROM + Wishbone 适配器的协议转换
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/ROM.h>
#include <dcs/components/WishboneROM.h>
#include <cstring>

using namespace dsc;

// ============================================================
// 共用夹具：ROM (内存模式 8b×256B) + WishboneROMAdapter + 所有网表
// ============================================================
struct WBROMFixture {
    std::unique_ptr<Circuit> circuit;
    ROM *rom = nullptr;
    WishboneROMAdapter *adapter = nullptr;

    Net *clk, *rst;
    // Wishbone 主→从
    Net *wb_adr, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc;
    // Wishbone 从→主
    Net *wb_dat_o, *wb_ack;
    // ROM ↔ 适配器
    Net *rom_addr, *rom_data;

    static constexpr int ADDR_W = 8;  // 256 字节
    static constexpr int DATA_W = 32; // 32 位
    static constexpr int LATENCY = 2;

    WBROMFixture(int latency = LATENCY) {
        circuit = std::make_unique<Circuit>();

        // ROM 先加入 → _id 小 → 先执行
        auto r = std::make_unique<ROM>("rom0", ADDR_W, DATA_W, latency);
        rom = r.get();
        circuit->addComponent(std::move(r));

        // 适配器后加入 → _id 大 → 后执行
        auto a = std::make_unique<WishboneROMAdapter>("adapter0", ADDR_W, DATA_W, latency);
        adapter = a.get();
        circuit->addComponent(std::move(a));

        // 创建网表
        clk   = circuit->createNet("clk");
        rst   = circuit->createNet("rst");
        wb_adr   = circuit->createNet("wb_adr");
        wb_dat_i = circuit->createNet("wb_dat_i");
        wb_we    = circuit->createNet("wb_we");
        wb_sel   = circuit->createNet("wb_sel");
        wb_stb   = circuit->createNet("wb_stb");
        wb_cyc   = circuit->createNet("wb_cyc");
        wb_dat_o = circuit->createNet("wb_dat_o");
        wb_ack   = circuit->createNet("wb_ack");
        rom_addr = circuit->createNet("rom_addr");
        rom_data = circuit->createNet("rom_data");

        // ROM 连接
        circuit->connect(rom, "addr", rom_addr);
        circuit->connect(rom, "data_out", rom_data);
        circuit->connect(rom, "clk", clk);

        // 适配器 Wishbone 侧
        circuit->connect(adapter, "wb_adr_i", wb_adr);
        circuit->connect(adapter, "wb_dat_i", wb_dat_i);
        circuit->connect(adapter, "wb_we_i", wb_we);
        circuit->connect(adapter, "wb_sel_i", wb_sel);
        circuit->connect(adapter, "wb_stb_i", wb_stb);
        circuit->connect(adapter, "wb_cyc_i", wb_cyc);
        circuit->connect(adapter, "wb_dat_o", wb_dat_o);
        circuit->connect(adapter, "wb_ack_o", wb_ack);
        circuit->connect(adapter, "clk", clk);

        // 适配器 ROM 侧
        circuit->connect(adapter, "rom_addr", rom_addr);
        circuit->connect(adapter, "rom_data", rom_data);
    }

    void writeROM(size_t offset, const std::vector<uint8_t> &bytes) {
        uint8_t *m = rom->writableMemPtr();
        ASSERT_NE(m, nullptr) << "ROM 非内存模式，无法写入";
        ASSERT_LE(offset + bytes.size(), rom->memSize());
        std::memcpy(m + offset, bytes.data(), bytes.size());
    }

    void writeROM32(size_t byte_offset, uint32_t val) {
        std::vector<uint8_t> b(4);
        b[0] = (uint8_t)(val & 0xFF);
        b[1] = (uint8_t)((val >> 8) & 0xFF);
        b[2] = (uint8_t)((val >> 16) & 0xFF);
        b[3] = (uint8_t)((val >> 24) & 0xFF);
        writeROM(byte_offset, b);
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
    }

    // 驱动一条 Wishbone 信号线（值取低字节）
    void setWb(Net *net, uint64_t val) {
        std::vector<uint8_t> v = {(uint8_t)val};
        circuit->setWire(net->id(), v);
    }

    // 读一条 Wishbone 信号线（1-bit）
    uint64_t getWb(Net *net) {
        auto v = circuit->getWire(net->id());
        return v.empty() ? 0 : v[0];
    }

    // 读取多字节线网值（小端）
    uint64_t getWbData(Net *net) {
        auto v = circuit->getWire(net->id());
        uint64_t val = 0;
        for (size_t i = 0; i < v.size() && i < 8; i++)
            val |= (uint64_t)v[i] << (i * 8);
        return val;
    }

    // 半个时钟周期
    void halfTick(bool clk_val) {
        circuit->setWire(clk->id(), {clk_val ? 1ULL : 0ULL});
        circuit->tick();
    }

    // 一个完整时钟周期（低→高）
    void fullTick() {
        halfTick(false);
        halfTick(true);
    }

    void reset() {
        setWb(clk, 0);
        setWb(rst, 1);
        setWb(wb_stb, 0);
        setWb(wb_cyc, 0);
        setWb(wb_adr, 0);
        setWb(wb_we, 0);
        setWb(wb_sel, 0);
        setWb(wb_dat_i, 0);
        circuit->tick();
        setWb(rst, 0);
    }

    // 发起 Wishbone 读事务，返回 {data, ack}
    struct ReadResult { uint64_t data; bool ack; };
    ReadResult doRead(uint64_t addr, int cycles = 20) {
        setWb(wb_adr, addr);
        setWb(wb_cyc, 1);
        setWb(wb_stb, 1);

        uint64_t rdata = 0;
        bool rack = false;
        for (int i = 0; i < cycles; i++) {
            fullTick();
            if (getWb(wb_ack)) {
                rdata = getWbData(wb_dat_o);
                rack = true;
                // 收到 ack → 撤销 stb
                setWb(wb_stb, 0);
                // 再给一拍让适配器看到 stb 下降
                fullTick();
                break;
            }
        }
        setWb(wb_cyc, 0);
        setWb(wb_stb, 0);
        return {rdata, rack};
    }
};

// ============================================================
// 基本读取：写入 4 字节 → Wishbone 读回
// ============================================================
TEST(WBROMTest, BasicRead) {
    WBROMFixture f(2); // latency=2

    // 写入 ROM 0x00: 0xDEADBEEF
    f.writeROM32(0x00, 0xDEADBEEFu);
    // 写入 ROM 0x04: 0xCAFEBABEu
    f.writeROM32(0x04, 0xCAFEBABEu);

    f.compile();
    f.reset();

    // 读地址 0
    auto r = f.doRead(0x00);
    EXPECT_TRUE(r.ack);
    EXPECT_EQ(r.data, 0xDEADBEEFu);

    // 读地址 4
    r = f.doRead(0x04);
    EXPECT_TRUE(r.ack);
    EXPECT_EQ(r.data, 0xCAFEBABEu);
}

// ============================================================
// 延迟 0：当拍响应
// ============================================================
TEST(WBROMTest, LatencyZero) {
    WBROMFixture f(0); // latency=0

    f.writeROM32(0x00, 0x12345678u);
    f.compile();
    f.reset();

    auto r = f.doRead(0x00);
    EXPECT_TRUE(r.ack);
    EXPECT_EQ(r.data, 0x12345678u);
}

// ============================================================
// 延迟 3：多拍等待
// ============================================================
TEST(WBROMTest, Latency3) {
    WBROMFixture f(3); // latency=3

    f.writeROM32(0x10, 0xAABBCCDDu);
    f.compile();
    f.reset();

    auto r = f.doRead(0x10);
    EXPECT_TRUE(r.ack);
    EXPECT_EQ(r.data, 0xAABBCCDDu);
}

// ============================================================
// 无 stb 时不应有 ack
// ============================================================
TEST(WBROMTest, NoStbNoAck) {
    WBROMFixture f(1);

    f.writeROM32(0x00, 0xDEADu);
    f.compile();
    f.reset();

    // cyc=1 但 stb=0 → 不应有 ack
    f.setWb(f.wb_cyc, 1);
    f.setWb(f.wb_stb, 0);
    f.setWb(f.wb_adr, 0x00);

    for (int i = 0; i < 10; i++) {
        f.fullTick();
        EXPECT_EQ(f.getWb(f.wb_ack), 0u) << "ack 不应为高，tick=" << i;
    }
}

// ============================================================
// 连续读取：第二个请求在第一个完成后立即发起
// ============================================================
TEST(WBROMTest, SequentialReads) {
    WBROMFixture f(2);

    f.writeROM32(0x00, 111u);
    f.writeROM32(0x04, 222u);
    f.writeROM32(0x08, 333u);
    f.compile();
    f.reset();

    auto r1 = f.doRead(0x00);
    EXPECT_TRUE(r1.ack);
    EXPECT_EQ(r1.data, 111u);

    auto r2 = f.doRead(0x04);
    EXPECT_TRUE(r2.ack);
    EXPECT_EQ(r2.data, 222u);

    auto r3 = f.doRead(0x08);
    EXPECT_TRUE(r3.ack);
    EXPECT_EQ(r3.data, 333u);
}

// ============================================================
// 地址边界检查：超出 ROM 容量的地址应返回 0
// ============================================================
TEST(WBROMTest, OutOfRangeReturnsZero) {
    WBROMFixture f(1);

    f.writeROM32(0x00, 0xFFFFFFFFu);
    f.compile();
    f.reset();

    // addr=0x80 未写入数据 → 应返回 0
    auto r = f.doRead(0x80);
    EXPECT_TRUE(r.ack);
    EXPECT_EQ(r.data, 0u);
}

// ============================================================
// 不同数据位宽：8-bit
// ============================================================
TEST(WBROMTest, DataWidth8) {
    auto circuit = std::make_unique<Circuit>();

    auto r = std::make_unique<ROM>("rom8", 8, 8, 1);
    auto *rom = r.get();
    circuit->addComponent(std::move(r));

    auto a = std::make_unique<WishboneROMAdapter>("ad8", 8, 8, 1);
    auto *adapter = a.get();
    circuit->addComponent(std::move(a));

    auto clk = circuit->createNet("clk");
    auto wb_adr = circuit->createNet("adr");
    auto wb_stb = circuit->createNet("stb");
    auto wb_cyc = circuit->createNet("cyc");
    auto wb_ack = circuit->createNet("ack");
    auto wb_dat_o = circuit->createNet("dat_o");
    auto rom_addr = circuit->createNet("radr");
    auto rom_data = circuit->createNet("rdat");
    auto dummy = circuit->createNet("dummy"); // 未使用的 WB 引脚也连上

    circuit->connect(rom, "addr", rom_addr);
    circuit->connect(rom, "data_out", rom_data);
    circuit->connect(rom, "clk", clk);
    circuit->connect(adapter, "wb_adr_i", wb_adr);
    circuit->connect(adapter, "wb_dat_i", dummy);
    circuit->connect(adapter, "wb_we_i", dummy);
    circuit->connect(adapter, "wb_sel_i", dummy);
    circuit->connect(adapter, "wb_stb_i", wb_stb);
    circuit->connect(adapter, "wb_cyc_i", wb_cyc);
    circuit->connect(adapter, "wb_dat_o", wb_dat_o);
    circuit->connect(adapter, "wb_ack_o", wb_ack);
    circuit->connect(adapter, "rom_addr", rom_addr);
    circuit->connect(adapter, "rom_data", rom_data);
    circuit->connect(adapter, "clk", clk);

    // 写入 ROM
    uint8_t *m = rom->writableMemPtr();
    m[0x00] = 0xAB;
    m[0x01] = 0xCD;
    m[0xFF] = 0xEF;

    auto err = circuit->compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    circuit->init();

    // 复位
    circuit->setWire(clk->id(), {0});
    circuit->tick();

    auto doRead8 = [&](uint64_t addr) -> uint64_t {
        circuit->setWire(wb_adr->id(), {(uint8_t)addr});
        circuit->setWire(wb_cyc->id(), {1});
        circuit->setWire(wb_stb->id(), {1});
        uint64_t data = 0x100; // 哨兵值
        for (int i = 0; i < 20; i++) {
            circuit->setWire(clk->id(), {0}); circuit->tick();
            circuit->setWire(clk->id(), {1}); circuit->tick();
            auto ack_vec = circuit->getWire(wb_ack->id());
            if (!ack_vec.empty() && ack_vec[0]) {
                auto dat_vec = circuit->getWire(wb_dat_o->id());
                data = dat_vec.empty() ? 0 : dat_vec[0];
                circuit->setWire(wb_stb->id(), {0});
                circuit->setWire(clk->id(), {0}); circuit->tick();
                circuit->setWire(clk->id(), {1}); circuit->tick();
                break;
            }
        }
        circuit->setWire(wb_cyc->id(), {0});
        circuit->setWire(wb_stb->id(), {0});
        return data & 0xFF;
    };

    EXPECT_EQ(doRead8(0x00), 0xABu);
    EXPECT_EQ(doRead8(0x01), 0xCDu);
    EXPECT_EQ(doRead8(0xFF), 0xEFu);
}

// ============================================================
// 16-bit 数据位宽
// ============================================================
TEST(WBROMTest, DataWidth16) {
    auto circuit = std::make_unique<Circuit>();

    auto r = std::make_unique<ROM>("rom16", 8, 16, 1);
    auto *rom = r.get();
    circuit->addComponent(std::move(r));

    auto a = std::make_unique<WishboneROMAdapter>("ad16", 8, 16, 1);
    auto *adapter = a.get();
    circuit->addComponent(std::move(a));

    auto clk    = circuit->createNet("clk");
    auto wb_adr = circuit->createNet("adr");
    auto wb_stb = circuit->createNet("stb");
    auto wb_cyc = circuit->createNet("cyc");
    auto wb_ack = circuit->createNet("ack");
    auto wb_do  = circuit->createNet("dat_o");
    auto radr   = circuit->createNet("radr");
    auto rdat   = circuit->createNet("rdat");
    auto dmy    = circuit->createNet("dmy");

    circuit->connect(rom, "addr", radr);
    circuit->connect(rom, "data_out", rdat);
    circuit->connect(rom, "clk", clk);
    circuit->connect(adapter, "wb_adr_i", wb_adr);
    circuit->connect(adapter, "wb_dat_i", dmy);
    circuit->connect(adapter, "wb_we_i", dmy);
    circuit->connect(adapter, "wb_sel_i", dmy);
    circuit->connect(adapter, "wb_stb_i", wb_stb);
    circuit->connect(adapter, "wb_cyc_i", wb_cyc);
    circuit->connect(adapter, "wb_dat_o", wb_do);
    circuit->connect(adapter, "wb_ack_o", wb_ack);
    circuit->connect(adapter, "rom_addr", radr);
    circuit->connect(adapter, "rom_data", rdat);
    circuit->connect(adapter, "clk", clk);

    // 小端写入 16-bit 值
    uint8_t *m = rom->writableMemPtr();
    m[0] = 0x34; m[1] = 0x12; // 0x1234 @ addr 0
    m[2] = 0xBC; m[3] = 0x9A; // 0x9ABC @ addr 2

    auto err = circuit->compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    circuit->init();
    circuit->setWire(clk->id(), {0});
    circuit->tick();

    auto read16 = [&](uint64_t addr) -> uint64_t {
        circuit->setWire(wb_adr->id(), {(uint8_t)addr});
        circuit->setWire(wb_cyc->id(), {1});
        circuit->setWire(wb_stb->id(), {1});
        uint64_t data = 0xFFFF; // 哨兵
        for (int i = 0; i < 20; i++) {
            circuit->setWire(clk->id(), {0}); circuit->tick();
            circuit->setWire(clk->id(), {1}); circuit->tick();
            auto ack_vec2 = circuit->getWire(wb_ack->id());
            if (!ack_vec2.empty() && ack_vec2[0]) {
                auto dat_vec2 = circuit->getWire(wb_do->id());
                uint64_t raw16 = 0;
                for (size_t j = 0; j < dat_vec2.size() && j < 2; j++)
                    raw16 |= (uint64_t)dat_vec2[j] << (j * 8);
                data = raw16;
                circuit->setWire(wb_stb->id(), {0});
                circuit->setWire(clk->id(), {0}); circuit->tick();
                circuit->setWire(clk->id(), {1}); circuit->tick();
                break;
            }
        }
        circuit->setWire(wb_cyc->id(), {0});
        circuit->setWire(wb_stb->id(), {0});
        return data & 0xFFFF;
    };

    EXPECT_EQ(read16(0x00), 0x1234u);
    EXPECT_EQ(read16(0x02), 0x9ABCu);
}

// ============================================================
// 调试：单独测试 ROM 流水线（不用适配器）
// ============================================================
