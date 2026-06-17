// MultiPortRAM 测试：基本读写 / 2R1W 寄存器堆 / 读流水线延迟 / rd_valid 流控
#include <dcs/Circuit.h>
#include <dcs/components/MultiPortRAM.h>
#include <gtest/gtest.h>
#include <memory>

// 辅助：构建 nR/nW 的 RAM，编译，初始化
static std::unique_ptr<dsc::Circuit> build(int addr_w, int data_w, int nr, int nw, int rd_lat = 0) {
    auto c = std::make_unique<dsc::Circuit>();
    auto ram = std::make_unique<dsc::MultiPortRAM>("mem", addr_w, data_w, nr, nw, rd_lat);
    auto *rp = c->addComponent(std::move(ram));
    for (auto &in: rp->inputs()) {
        auto *net = c->createNet(in->name());
        c->connect(rp, in->name(), net);
    }
    for (auto &out: rp->outputs()) {
        auto *net = c->createNet(out->name());
        c->connect(rp, out->name(), net);
    }
    auto err = c->compile();
    if (!c->isCompiled())
        ADD_FAILURE() << "编译失败: " << err.message;
    c->init();
    return c;
}

// 引脚布局（以 1R1W 为例）：
//   输入: [0]clk, [1]wr_addr_0, [2]wr_data_0, [3]we_0, [4]rd_addr_0
//   输出: [0]rd_data_0, [1]rd_valid
//   线网总数 = 5 + 2 = 7: nets[0..4]=输入, nets[5]=rd_data_0, nets[6]=rd_valid

// ============================================================
// 1R1W — 基本读写 + rd_valid
// ============================================================
TEST(MultiPortRAM, Basic1R1W) {
    auto c = build(4, 8, 1, 1); // 16x8, 1R1W, rd_lat=0
    const auto &nets = c->nets();

    // rd_lat=0 → rd_valid 始终为 1
    c->tick();
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 1); // rd_valid=1

    // 写入地址 3
    c->setWire(nets[0]->id(), {1, 0}); // clk=1
    c->setWire(nets[1]->id(), {3, 0}); // wa_0=3
    c->setWire(nets[2]->id(), {0xAB, 0}); // wd_0=0xAB
    c->setWire(nets[3]->id(), {1, 0}); // we_0=1
    c->tick(); // 上升沿写入

    // 下拍读
    c->setWire(nets[0]->id(), {0, 0});
    c->setWire(nets[4]->id(), {3, 0}); // ra_0=3
    c->tick();
    EXPECT_EQ(c->getWire(nets[5]->id())[0], 0xAB);
}

// ============================================================
// read_latency=1 — 同步读 + rd_valid 在前 N 拍为 0
// ============================================================
TEST(MultiPortRAM, ReadLatency1) {
    auto c = build(4, 8, 1, 1, 1); // rd_lat=1
    const auto &nets = c->nets();

    // 写入
    c->setWire(nets[0]->id(), {1, 0});
    c->setWire(nets[1]->id(), {5, 0}); // wa=5
    c->setWire(nets[2]->id(), {0xCD, 0}); // wd=0xCD
    c->setWire(nets[3]->id(), {1, 0}); // we=1
    c->tick();

    // 第1拍 clk=1, ra=5 — rd_valid 应该为 0（流水线未满）
    c->setWire(nets[0]->id(), {1, 0});
    c->setWire(nets[4]->id(), {5, 0});
    c->tick();
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 0); // rd_valid=0，数据无效

    // 第2拍 clk=0 — rd_valid 变为 1，数据有效
    c->setWire(nets[0]->id(), {0, 0});
    c->tick();
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 1); // rd_valid=1
    EXPECT_EQ(c->getWire(nets[5]->id())[0], 0xCD);
}

// ============================================================
// read_latency=3 — 更深的流水线（需 5 tick 才出数据）
// ============================================================
TEST(MultiPortRAM, ReadLatency3) {
    auto c = build(4, 8, 1, 1, 3); // rd_lat=3
    const auto &nets = c->nets();

    // Tick 1: 写入 mem[9]=0x99
    c->setWire(nets[0]->id(), {1, 0});
    c->setWire(nets[1]->id(), {9, 0});
    c->setWire(nets[2]->id(), {0x99, 0});
    c->setWire(nets[3]->id(), {1, 0});
    c->tick();

    // 从 tick 2 开始读地址 9
    c->setWire(nets[4]->id(), {9, 0});
    c->setWire(nets[0]->id(), {0, 0});

    c->tick(); // tick 2
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 0);

    c->tick(); // tick 3
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 0);

    c->tick(); // tick 4
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 0);

    c->tick(); // tick 5: rd_valid=1，数据出现
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 1);
    EXPECT_EQ(c->getWire(nets[5]->id())[0], 0x99);
}

// ============================================================
// 2R1W — 寄存器堆模式
// 输入: [0]clk, [1]wa, [2]wd, [3]we, [4]ra0, [5]ra1
// 输出: [0]rd0, [1]rd1, [2]rd_valid
// ============================================================
TEST(MultiPortRAM, RegFile2R1W) {
    auto c = build(4, 8, 2, 1); // 16x8, 2R1W
    const auto &nets = c->nets();

    // 写 reg[1]=0x11
    c->setWire(nets[0]->id(), {1, 0}); // clk↑
    c->setWire(nets[1]->id(), {1, 0});
    c->setWire(nets[2]->id(), {0x11, 0});
    c->setWire(nets[3]->id(), {1, 0}); // we=1
    c->tick();
    // clk 归零再拉高，产生第二个上升沿
    c->setWire(nets[0]->id(), {0, 0});
    c->tick();
    // 写 reg[2]=0x22
    c->setWire(nets[0]->id(), {1, 0});
    c->setWire(nets[1]->id(), {2, 0});
    c->setWire(nets[2]->id(), {0x22, 0});
    c->tick();

    // rd_lat=0，同时读两端口
    c->setWire(nets[0]->id(), {0, 0});
    c->setWire(nets[4]->id(), {1, 0}); // ra0=1 → rd_data_0 = nets[6]
    c->setWire(nets[5]->id(), {2, 0}); // ra1=2 → rd_data_1 = nets[7]
    c->tick();
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 0x11);
    EXPECT_EQ(c->getWire(nets[7]->id())[0], 0x22);
    EXPECT_EQ(c->getWire(nets[8]->id())[0], 1); // rd_valid=1
}

// ============================================================
// 1R2W — 双写端口，同时写同地址（后者赢）
// 输入: [0]clk, [1]wa0, [2]wd0, [3]we0, [4]wa1, [5]wd1, [6]we1, [7]ra0
// 输出: [0]rd0, [1]rd_valid
// ============================================================
TEST(MultiPortRAM, DualWriteSameAddr) {
    auto c = build(4, 8, 1, 2);
    const auto &nets = c->nets();

    // 两端口同时写 addr=0 不同值
    c->setWire(nets[0]->id(), {1, 0}); // clk
    c->setWire(nets[1]->id(), {0, 0}); // wa0=0
    c->setWire(nets[2]->id(), {0xAA, 0}); // wd0=0xAA
    c->setWire(nets[3]->id(), {1, 0}); // we0=1
    c->setWire(nets[4]->id(), {0, 0}); // wa1=0
    c->setWire(nets[5]->id(), {0x55, 0}); // wd1=0x55
    c->setWire(nets[6]->id(), {1, 0}); // we1=1
    c->tick(); // port1 后执行，覆盖 port0

    c->setWire(nets[0]->id(), {0, 0});
    c->tick();
    EXPECT_EQ(c->getWire(nets[8]->id())[0], 0x55); // rd_data_0
}

// ============================================================
// 多写端口不同地址，互不干扰
// ============================================================
TEST(MultiPortRAM, DualWriteDiffAddr) {
    auto c = build(4, 8, 1, 2);
    const auto &nets = c->nets();

    c->setWire(nets[0]->id(), {1, 0});
    c->setWire(nets[1]->id(), {3, 0}); // wa0=3
    c->setWire(nets[2]->id(), {0x33, 0});
    c->setWire(nets[3]->id(), {1, 0}); // we0=1
    c->setWire(nets[4]->id(), {7, 0}); // wa1=7
    c->setWire(nets[5]->id(), {0x77, 0});
    c->setWire(nets[6]->id(), {1, 0}); // we1=1
    c->tick();

    c->setWire(nets[0]->id(), {0, 0});
    c->setWire(nets[7]->id(), {3, 0});
    c->tick();
    EXPECT_EQ(c->getWire(nets[8]->id())[0], 0x33);

    c->setWire(nets[7]->id(), {7, 0});
    c->tick();
    EXPECT_EQ(c->getWire(nets[8]->id())[0], 0x77);
}

// ============================================================
// 组合读（rd_lat=0）：rd_valid 始终 1，初始值 0
// ============================================================
TEST(MultiPortRAM, CombReadSeesOldValue) {
    auto c = build(4, 8, 1, 1, 0);
    const auto &nets = c->nets();

    // 不写任何数据，直接读
    c->setWire(nets[0]->id(), {0, 0});
    c->setWire(nets[4]->id(), {0, 0});
    c->tick();
    EXPECT_EQ(c->getWire(nets[5]->id())[0], 0); // 初始为 0
    EXPECT_EQ(c->getWire(nets[6]->id())[0], 1); // rd_valid=1
}
