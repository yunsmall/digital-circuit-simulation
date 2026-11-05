// RV32 测试共用的指令编码。
// 每个测试文件各写一份容易出现同一指令两种编码（以前 FCVT 的 funct7 就写错过），
// 统一放在这里，新增编码只改这一处。
#pragma once

#include <cstdint>

namespace rv32enc {

// 通用 I/R 型（立即数按 12 位截取）
constexpr uint32_t i_type(int opcode, int rd, int f3, int rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(f3 & 7) << 12) | ((uint32_t)(rd & 0x1F) << 7)
         | (uint32_t)(opcode & 0x7F);
}
constexpr uint32_t r_type(int opcode, int rd, int f3, int rs1, int rs2, int f7) {
    return ((uint32_t)(f7 & 0x7F) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | ((uint32_t)(f3 & 7) << 12)
         | ((uint32_t)(rd & 0x1F) << 7) | (uint32_t)(opcode & 0x7F);
}

// ---- 整数 ----
constexpr uint32_t addi(int rd, int rs1, int32_t imm) { return i_type(0x13, rd, 0, rs1, imm); }
constexpr uint32_t lui(int rd, uint32_t imm20) {
    return (imm20 << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x37u;
}
constexpr uint32_t lw(int rd, int rs1, int32_t imm) { return i_type(0x03, rd, 2, rs1, imm); }
// STORE 的 imm[11:5] 在 inst[31:25]，rs2 在 inst[24:20]
constexpr uint32_t sw(int rs2, int rs1, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | (2u << 12) | ((uint32_t)(imm & 0x1F) << 7) | 0x23u;
}
constexpr uint32_t jal(int rd, int32_t off) {
    const uint32_t u = (uint32_t) off;
    return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 1u) << 20)
         | (((u >> 12) & 0xFF) << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x6Fu;
}
// 自旋：jal x0, 0。程序末尾必须自旋，否则会继续执行后面的 0 字节（非法指令），
// 进陷阱后若 mtvec 还是 0 就会从头重跑，测试看到的是重复执行后的状态
constexpr uint32_t spin() { return 0x0000006Fu; }
constexpr uint32_t amo(int rd, int rs1, int rs2, uint32_t funct5) {
    return (funct5 << 27) | ((uint32_t)(rs2 & 0x1F) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | (2u << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x2Fu;
}

// ---- CSR ----
constexpr uint32_t csrrw(int rd, int csr, int rs1) { return i_type(0x73, rd, 1, rs1, csr); }
constexpr uint32_t csrrs(int rd, int csr, int rs1) { return i_type(0x73, rd, 2, rs1, csr); }

// ---- 浮点访存 ----
constexpr uint32_t flw(int rd, int rs1, int32_t imm) { return i_type(0x07, rd, 2, rs1, imm); }
constexpr uint32_t flh(int rd, int rs1, int32_t imm) { return i_type(0x07, rd, 1, rs1, imm); }
constexpr uint32_t fld(int rd, int rs1, int32_t imm) { return i_type(0x07, rd, 3, rs1, imm); }
constexpr uint32_t fsw(int rs2, int rs1, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | (2u << 12) | ((uint32_t)(imm & 0x1F) << 7) | 0x27u;
}
constexpr uint32_t fsd(int rs2, int rs1, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | (3u << 12) | ((uint32_t)(imm & 0x1F) << 7) | 0x27u;
}

// ---- 浮点运算 ----
// funct7 的低两位是格式域（0=S 1=D 2=H），调用点按需要拼进 f7
constexpr uint32_t fp(uint32_t f7, int rd, int rs1, int rs2) {
    return (f7 << 25) | ((uint32_t)(rs2 & 0x1F) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(rd & 0x1F) << 7) | 0x53u;
}
// 只带 rs1 的浮点位（FCLASS/FMV.X.W 等）
constexpr uint32_t fp_f3(uint32_t f7, int f3, int rd, int rs1) {
    return (f7 << 25) | ((uint32_t)(f3 & 7) << 12) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(rd & 0x1F) << 7) | 0x53u;
}
// 带 rs2 和 funct3 的浮点 R 型：FMIN/FMAX 以及带静态舍入模式的运算
constexpr uint32_t fp_r(uint32_t f7, int f3, int rd, int rs1, int rs2) {
    return (f7 << 25) | ((uint32_t)(rs2 & 0x1F) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(f3 & 7) << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x53u;
}
// FMADD/FMSUB/FNMSUB/FNMADD：fmt 位在 instr[26:25]，rs3 在 instr[31:27]
constexpr uint32_t fma(uint32_t op, int fmt, int rd, int rs1, int rs2, int rs3, int rm) {
    return ((uint32_t)(rs3 & 0x1F) << 27) | ((uint32_t)(fmt & 3) << 25)
         | ((uint32_t)(rs2 & 0x1F) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(rm & 7) << 12) | ((uint32_t)(rd & 0x1F) << 7) | (op & 0x7F);
}
constexpr uint32_t fma_d(uint32_t op, int rd, int rs1, int rs2, int rs3, int rm) {
    return fma(op, 1, rd, rs1, rs2, rs3, rm);
}
// 浮点→整数 / 整数→浮点的转换（FCVT.W.S、FCVT.S.W 等）：funct5=0x1A，
// rs2 选目标/源宽度
constexpr uint32_t fcvt(int rd, int rs1, int rm, int rs2) {
    return (0x68u << 25) | ((uint32_t)(rs2 & 0x1F) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
           | ((uint32_t)(rm & 7) << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x53u;
}
// FCVT.W/WU 用 funct5=0x18（funct7=0x60|fmt），与上面的 FCVT.S.W 家族不同。
// fmt 是源格式：S=0、D=1
constexpr uint32_t fcvt_w(int rd, int rs1, int rm, int rs2, int fmt = 0) {
    return ((0x60u | (uint32_t)(fmt & 3)) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
           | ((uint32_t)(rs1 & 0x1F) << 15) | ((uint32_t)(rm & 7) << 12)
           | ((uint32_t)(rd & 0x1F) << 7) | 0x53u;
}
// 浮点→浮点转换（FCVT.S.D / FCVT.H.S 等）：funct5=0x08，低两位选目标格式
// （S=0,D=1,H=2），rs2 选源格式。和整数转换的 0x68 不是一回事
constexpr uint32_t fcvt_fp(int dst, int src, int rd, int rs1, int rm = 0) {
    return (((0x20u | (uint32_t)(dst & 3)) << 25) | ((uint32_t)(src & 0x1F) << 20)
            | ((uint32_t)(rs1 & 0x1F) << 15) | ((uint32_t)(rm & 7) << 12)
            | ((uint32_t)(rd & 0x1F) << 7) | 0x53u);
}

} // namespace rv32enc
