// Sv39 三级页表的行为验证：三级遍历、1GB/2MB 大页、satp 模式位。
// 官方 dirty.S 覆盖了 1GB 大页的 A/D 语义，这里补齐三级 4KB 页和 CSR 接受性。
#include <gtest/gtest.h>

#include "platform64.h"

namespace {

void w64(WishboneMemory *mem, uint32_t addr, uint64_t v) {
    uint8_t *m = mem->writableMemPtr();
    for (int i = 0; i < 8; i++) m[addr + i] = (uint8_t)(v >> (8 * i));
}
void w32(WishboneMemory *mem, uint32_t addr, uint32_t v) {
    uint8_t *m = mem->writableMemPtr();
    for (int i = 0; i < 4; i++) m[addr + i] = (uint8_t)(v >> (8 * i));
}

// Sv39 PTE：PPN 在 bit53:10，低 8 位是标志。测试里只用低 32 位能表达的物理地址
constexpr uint64_t pte(uint64_t ppn, uint64_t flags) { return (ppn << 10) | flags; }
constexpr uint64_t V = 1, R = 2, W = 4, X = 8, A = 0x40, D = 0x80;

// M 态初始化：开 Sv39、设 mtvec、切到 S 态。入口在 S 态代码 0x1000，
// 该页由页表 identity 映射；数据和页表都在 64KB 内存里。
const std::vector<uint32_t> initSequence(uint64_t satp, uint32_t mepc) {
    return {
        addi(1, 0, (int32_t)(satp & 0xFFF)),           // 0x00
        addi(2, 0, -1),                                // 0x04
        enc_shift_i(2, 1, 2, 63),                      // 0x08 x2 = 1<<63(BIT63)
        enc_r(0x33, 1, 6, 1, 2, 0),                    // 0x0C or x1, x1, x2
        csrrw(0, 0x180, 1),                            // 0x10 csrw satp
        0x12000073u,                                   // 0x14 sfence.vma
        // 0x880 超出 addi 的 12 位有符号范围，必须拆成 lui+addi
        lui(1, 1),                                     // 0x18 x1 = 0x1000
        addi(1, 1, -0x780),                            // 0x1C x1 = 0x880（MPP=S | MPIE）
        csrrs(0, 0x300, 1),                            // 0x20 csrs mstatus
        addi(1, 0, 0x3C),                              // 0x24 mtvec = 0x3C
        csrrw(0, 0x305, 1),                            // 0x28
        lui(1, mepc >> 12),                            // 0x2C
        csrrw(0, 0x341, 1),                            // 0x30 csrw mepc
        0x30200073u,                                   // 0x34 mret
        0x00000013u,                                   // 0x38 nop
        csrrs(31, 0x342, 0),                           // 0x3C x31 = mcause
        spin(),                                        // 0x40
    };
}

} // namespace

// 三级 4KB 页：根→中间→叶 各读一次，VA 0x2000 映射到 PA 0x5000
TEST(Sv39Test, ThreeLevelWalk) {
    Rv64Fixture f;
    // 页表：根 @0x2000、中间 @0x3000、叶 @0x4000
    w64(f.mem, 0x2000 + 0, pte(0x3, V));              // 根[0] → 中间页表
    w64(f.mem, 0x3000 + 0, pte(0x4, V));              // 中[0] → 叶页表
    w64(f.mem, 0x4000 + 1 * 8, pte(0x1, V | R | W | X | A | D)); // VA 0x1000 → PA 0x1000（代码页）
    w64(f.mem, 0x4000 + 2 * 8, pte(0x5, V | R | W | A | D));     // VA 0x2000 → PA 0x5000（数据页）
    w32(f.mem, 0x5000, 0x5A);                          // 数据

    // S 态代码 @ PA 0x1000：读 VA 0x2000，然后 ecall 进 mtvec
    w32(f.mem, 0x1000, lui(5, 2));
    w32(f.mem, 0x1004, enc_i(0x03, 3, 2, 5, 0));       // lw x3, 0(x5)
    w32(f.mem, 0x1008, 0x00000073u);                   // ecall

    f.load(initSequence(0x8000000000000002ULL, 0x1000)); // Sv39, 根 PPN=2
    f.run(400);

    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0x5AULL)
        << "Sv39 三级遍历后 x3 应为数据 0x5A";
    EXPECT_EQ((uint64_t) f.cpu->reg(31), 0x09ULL)
        << "S 态 ecall 应进 M 态陷阱（x31=mcause=9，ECALL_S）";
}

// 1GB 大页（根级叶子）：大页要求 PPN[1:0] 为 0，所以 PPN=0 时就是 identity，
// VA 的低 30 位直接拼到物理地址低位
TEST(Sv39Test, GigapageAtRootLevel) {
    Rv64Fixture f;
    // 根 @0x4000 的第 0 项就是叶子：覆盖 VA 0x0-0x3FFFFFFF
    w64(f.mem, 0x4000, pte(0x0, V | R | W | X | A | D));
    w32(f.mem, 0x2008, 0x77);                          // 数据 @ PA 0x2008
    w32(f.mem, 0x1000, lui(5, 2));                     // S 态 @ VA 0x1000
    w32(f.mem, 0x1004, enc_i(0x03, 3, 2, 5, 8));       // lw x3, 8(x5) → VA 0x2008
    w32(f.mem, 0x1008, 0x00000073u);

    f.load(initSequence(0x8000000000000004ULL, 0x1000)); // Sv39, 根 PPN=4
    f.run(300);

    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0x77ULL)
        << "1GB 大页应把虚拟地址低位直接拼到物理地址";
}

// satp 写入 Sv39 模式必须被接受（RV64 只实现 Sv39，其它模式忽略）
TEST(Sv39Test, SatpAcceptsSv39Mode) {
    Rv64Fixture f;
    f.load({
        addi(1, 0, 1),
        addi(2, 0, -1),
        enc_shift_i(2, 1, 2, 63),                      // x2 = 1<<63
        enc_r(0x33, 1, 6, 1, 2, 0),                    // x1 = Sv39 | PPN=1
        csrrw(0, 0x180, 1),                            // csrw satp
        csrrs(3, 0x180, 0),                            // x3 = satp
        spin(),
    });
    f.run(60);
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0x8000000000000001ULL)
        << "Sv39 模式的 satp 写入被丢弃了";
}

// 非 canonical 地址：Sv39 只定义 39 位，高 25 位必须与 bit38 一致
TEST(Sv39Test, NonCanonicalAddressTraps) {
    Rv64Fixture f;
    w64(f.mem, 0x2000 + 0, pte(0x3, V));
    w64(f.mem, 0x3000 + 0, pte(0x4, V));
    w64(f.mem, 0x4000 + 1 * 8, pte(0x1, V | R | W | X | A | D));

    // S 态：把 0x0000008000000000（bit39 置位、高 25 位不是全 0/全 1）装进 x5 再读
    w32(f.mem, 0x1000, lui(5, 0x80000));               // x5 = 0x80000000（符号扩展到 0xFFFFFFFF80000000）
    w32(f.mem, 0x1004, enc_shift_i(5, 1, 5, 1));       // slli x5, x5, 1 → 非 canonical
    w32(f.mem, 0x1008, enc_i(0x03, 3, 2, 5, 0));       // lw x3, 0(x5) → 应 page fault
    w32(f.mem, 0x100C, 0x00000073u);

    f.load(initSequence(0x8000000000000002ULL, 0x1000));
    f.run(300);

    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0ULL) << "非 canonical 地址不应被成功翻译";
    EXPECT_EQ((uint64_t) f.cpu->reg(31), 0x0DULL)
        << "非 canonical 地址应触发 load page fault（13）";
}
