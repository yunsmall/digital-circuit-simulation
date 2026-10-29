// RISC-V C 扩展（压缩指令）的解码：16 位编码 -> 等价的 32 位指令。
//
// 做成独立的纯函数而不是塞在 CPU 里，原因有两个：
// 1. 它只依赖编码格式，和总线、特权级、状态机都无关，可以单独测；
// 2. RV32 和 RV64 有一批同编码不同含义的指令（C.JAL/C.ADDIW、C.FLW/C.LD 等），
//    把差异收敛在 XLEN 这一个模板参数上，比在 CPU 里到处 if constexpr 干净得多。
//
// 调用方拿到 0 表示这是非法/未实现的压缩编码，应当报非法指令而不是静默跳过。
#pragma once
#include <cstdint>

namespace rvc {

// rd'/rs1'/rs2' 是 3 位，落在 x8~x15
constexpr uint32_t R3(uint32_t f) { return (f & 7u) + 8u; }

// CI 型立即数：imm[5] 在 bit12，imm[4:0] 在 bit6:2。
// 这是 6 位有符号数，必须符号扩展——不扩的话 C.ADDI 的负数立即数会变成大正数
constexpr int32_t imm_ci(uint32_t i) {
    const uint32_t raw = (((i >> 12) & 1u) << 5) | ((i >> 2) & 0x1Fu);
    return (int32_t)((raw << 26) >> 26);
}

// 分支型：imm[8|4:3] 在 bit12:10，imm[7:6|2:1|5] 在 bit6:2
constexpr int32_t imm_b(uint32_t i) {
    int32_t v = (int32_t)((((i >> 12) & 1u) << 8) | (((i >> 5) & 3u) << 6)
                        | (((i >> 2) & 1u) << 5) | (((i >> 10) & 3u) << 3)
                        | (((i >> 3) & 3u) << 1));
    return (v << 23) >> 23;   // 符号扩展到 9 位再回推
}

// J 型：imm[11|4|9:8|10|6|7|3:1|5]
constexpr int32_t imm_j(uint32_t i) {
    int32_t v = (int32_t)((((i >> 12) & 1u) << 11) | (((i >> 8) & 1u) << 10)
                        | (((i >> 9) & 3u) << 8) | (((i >> 6) & 1u) << 7)
                        | (((i >> 7) & 1u) << 6) | (((i >> 2) & 1u) << 5)
                        | (((i >> 11) & 1u) << 4) | (((i >> 3) & 7u) << 1));
    return (v << 20) >> 20;
}

// 组装常见格式
constexpr uint32_t R(int opcode, int rd, int f3, int rs1, int rs2, int f7) {
    return ((uint32_t)f7 << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15)
         | ((uint32_t)f3 << 12) | ((uint32_t)rd << 7) | (uint32_t)opcode;
}
constexpr uint32_t I(int opcode, int rd, int f3, int rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | ((uint32_t)rs1 << 15)
         | ((uint32_t)f3 << 12) | ((uint32_t)rd << 7) | (uint32_t)opcode;
}
constexpr uint32_t S(int f3, int rs1, int rs2, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((uint32_t)rs2 << 20)
         | ((uint32_t)rs1 << 15) | ((uint32_t)f3 << 12)
         | ((uint32_t)(imm & 0x1F) << 7) | 0x23u;
}
constexpr uint32_t B(int f3, int rs1, int rs2, int32_t imm) {
    return ((uint32_t)((imm >> 12) & 1) << 31) | ((uint32_t)((imm >> 5) & 0x3F) << 25)
         | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | ((uint32_t)f3 << 12)
         | ((uint32_t)((imm >> 1) & 0xF) << 8) | ((uint32_t)((imm >> 11) & 1) << 7) | 0x63u;
}
constexpr uint32_t J(int rd, int32_t imm) {
    return ((uint32_t)((imm >> 20) & 1) << 31) | ((uint32_t)((imm >> 1) & 0x3FF) << 21)
         | ((uint32_t)((imm >> 11) & 1) << 20) | ((uint32_t)((imm >> 12) & 0xFF) << 12)
         | ((uint32_t)rd << 7) | 0x6Fu;
}
constexpr uint32_t U(int opcode, int rd, int32_t imm20) {
    return ((uint32_t)imm20 & 0xFFFFF000u) | ((uint32_t)rd << 7) | (uint32_t)opcode;
}

// 非法编码返回 0，调用方据此报非法指令
constexpr uint32_t ILLEGAL = 0;

// 把一条 16 位压缩指令翻译成 32 位指令。
// XLEN 决定同一编码在 RV32/RV64 下的差别（C.JAL 与 C.ADDIW、C.FLW 与 C.LD 等）。
template<int XLEN>
uint32_t expand(uint16_t c) {
    const uint32_t op = c & 3u;
    const uint32_t f3 = (c >> 13) & 7u;
    const uint32_t b12 = (c >> 12) & 1u;
    const uint32_t rs1 = (c >> 7) & 0x1Fu;
    const uint32_t rs2 = (c >> 2) & 0x1Fu;
    const uint32_t rd = rs1, rd_p = R3(c >> 2), rs1_p = R3(c >> 7), rs2_p = R3(c >> 2);

    if (op == 0) {   // 象限 0
        switch (f3) {
        case 0: {    // C.ADDI4SPN -> addi rd', x2, nzuimm
            uint32_t u = (((c >> 11) & 3u) << 4) | (((c >> 7) & 0xFu) << 6)
                       | (((c >> 6) & 1u) << 2) | (((c >> 5) & 1u) << 3);
            if (u == 0) return ILLEGAL;
            return I(0x13, rd_p, 0, 2, (int32_t)u);
        }
        case 2: {    // C.LW -> lw rd', uimm(rs1')
            uint32_t u = (((c >> 10) & 7u) << 3) | (((c >> 6) & 1u) << 2) | (((c >> 5) & 1u) << 6);
            return I(0x03, rd_p, 2, rs1_p, (int32_t)u);
        }
        case 3: {    // RV32: C.FLW   RV64: C.LD
            uint32_t u = (((c >> 10) & 7u) << 3) | (((c >> 5) & 3u) << 6);
            if constexpr (XLEN == 32) return I(0x07, rd_p, 2, rs1_p, (int32_t)u);   // flw
            else return I(0x03, rd_p, 3, rs1_p, (int32_t)u);                        // ld
        }
        case 6: {    // C.SW -> sw rs2', uimm(rs1')
            uint32_t u = (((c >> 10) & 7u) << 3) | (((c >> 6) & 1u) << 2) | (((c >> 5) & 1u) << 6);
            return S(2, rs1_p, rs2_p, (int32_t)u);
        }
        case 7: {    // RV32: C.FSW   RV64: C.SD
            uint32_t u = (((c >> 10) & 7u) << 3) | (((c >> 5) & 3u) << 6);
            if constexpr (XLEN == 32) return S(2, rs1_p, rs2_p, (int32_t)u);        // fsw
            else return S(3, rs1_p, rs2_p, (int32_t)u);                             // sd
        }
        default: return ILLEGAL;   // C.FLD/C.FSD 属于 D 扩展，暂不支持
        }
    }

    if (op == 1) {   // 象限 1
        switch (f3) {
        case 0: {    // C.ADDI / C.NOP
            int32_t imm = imm_ci(c);
            return I(0x13, rd, 0, rd, imm);
        }
        case 1: {    // RV32: C.JAL   RV64: C.ADDIW
            if constexpr (XLEN == 32) return J(1, imm_j(c));
            else return I(0x1B, rd, 0, rd, imm_ci(c));
        }
        case 2:      // C.LI -> addi rd, x0, imm
            return I(0x13, rd, 0, 0, imm_ci(c));
        case 3: {    // rd==2: C.ADDI16SP；否则 C.LUI
            int32_t imm = imm_ci(c);
            if (rd == 2) {
                uint32_t u = (((c >> 12) & 1u) << 9) | (((c >> 6) & 1u) << 4)
                           | (((c >> 5) & 1u) << 6) | (((c >> 3) & 3u) << 7)
                           | (((c >> 2) & 1u) << 5);
                // u 是无符号数，直接 (u<<22)>>22 是逻辑右移、拿不到负的偏移；
                // 必须对 int32_t 做算术右移才是 10 位符号扩展
                int32_t simm = ((int32_t)(u << 22)) >> 22;
                if (simm == 0) return ILLEGAL;
                return I(0x13, 2, 0, 2, simm);
            }
            if (imm == 0) return ILLEGAL;
            // C.LUI 的立即数是 6 位有符号，左移 12 位后进 lui 的 imm[17:12]；
            // 符号必须先扩到 6 位，否则 0x3F 这类会变成负的 lui 而不是正的
            return U(0x37, rd, (((int32_t)((c >> 12) & 1u) << 5 | ((c >> 2) & 0x1Fu)) << 26 >> 26) * 4096);
        }
        case 4: {    // 算术/逻辑
            const uint32_t f2 = (c >> 10) & 3u;
            if (f2 == 0) {   // C.SRLI
                return I(0x13, rs1_p, 5, rs1_p, (int32_t)(((c >> 12) & 1u) << 5 | ((c >> 2) & 0x1Fu)));
            }
            if (f2 == 1) {   // C.SRAI
                return I(0x13, rs1_p, 5, rs1_p, (int32_t)(0x400 | (((c >> 12) & 1u) << 5) | ((c >> 2) & 0x1Fu)));
            }
            if (f2 == 2) {   // C.ANDI
                return I(0x13, rs1_p, 7, rs1_p, imm_ci(c));
            }
            const uint32_t f2b = (c >> 5) & 3u;
            const uint32_t f1 = (c >> 12) & 1u;
            if (f1 == 0) {   // C.SUB / C.XOR / C.OR / C.AND
                static const int f3t[4] = {0, 4, 6, 7};
                return R(0x33, rs1_p, f3t[f2b], rs1_p, rs2_p, (f2b == 0) ? 0x20 : 0x00);
            }
            // f1 == 1：RV64 的 C.SUBW/C.ADDW，RV32 保留
            if constexpr (XLEN == 64) {
                if (f2b == 0) return R(0x3B, rs1_p, 0, rs1_p, rs2_p, 0x20);   // subw
                if (f2b == 1) return R(0x3B, rs1_p, 0, rs1_p, rs2_p, 0x00);   // addw
            }
            return ILLEGAL;
        }
        case 5:      // C.J
            return J(0, imm_j(c));
        case 6:      // C.BEQZ -> beq rs1', x0, off
            return B(0, rs1_p, 0, imm_b(c));
        case 7:      // C.BNEZ -> bne rs1', x0, off
            return B(1, rs1_p, 0, imm_b(c));
        }
    }

    // 象限 2
    switch (f3) {
    case 0: {        // C.SLLI
        if (b12 == 0 && rd == 0) return ILLEGAL;
        return I(0x13, rd, 1, rd, (int32_t)(((c >> 12) & 1u) << 5 | ((c >> 2) & 0x1Fu)));
    }
    case 2: {        // C.LWSP -> lw rd, uimm(x2)；rd==0 是保留
        if (rd == 0) return ILLEGAL;
        uint32_t u = (((c >> 12) & 1u) << 5) | (((c >> 4) & 7u) << 2) | (((c >> 2) & 3u) << 6);
        return I(0x03, rd, 2, 2, (int32_t)u);
    }
    case 3: {        // RV32: C.FLWSP   RV64: C.LDSP
        uint32_t u = (((c >> 12) & 1u) << 5) | (((c >> 5) & 3u) << 3) | (((c >> 2) & 7u) << 6);
        if constexpr (XLEN == 32) return I(0x07, rd, 2, 2, (int32_t)u);
        else { if (rd == 0) return ILLEGAL; return I(0x03, rd, 3, 2, (int32_t)u); }
    }
    case 4: {
        const uint32_t b12v = (c >> 12) & 1u;
        const uint32_t rs2z = (c >> 2) & 0x1Fu;
        if (b12v == 0) {
            if (rs2z == 0) {          // C.JR -> jalr x0, 0(rs1)
                if (rd == 0) return ILLEGAL;
                return I(0x67, 0, 0, rd, 0);
            }
            return R(0x33, rd, 0, 0, rs2z, 0);   // C.MV -> add rd, x0, rs2
        }
        if (rs2z == 0) {
            if (rd == 0) return 0x00100073u;     // C.EBREAK
            return I(0x67, 1, 0, rd, 0);         // C.JALR -> jalr x1, 0(rs1)
        }
        return R(0x33, rd, 0, rd, rs2z, 0);      // C.ADD -> add rd, rd, rs2
    }
    case 6: {        // C.SWSP -> sw rs2, uimm(x2)
        uint32_t u = (((c >> 9) & 0xFu) << 2) | (((c >> 7) & 3u) << 6);
        return S(2, 2, rs2, (int32_t)u);
    }
    case 7: {        // RV32: C.FSWSP   RV64: C.SDSP
        uint32_t u = (((c >> 10) & 7u) << 3) | (((c >> 7) & 7u) << 6);
        if constexpr (XLEN == 32) return S(2, 2, rs2, (int32_t)u);
        else return S(3, 2, rs2, (int32_t)u);
    }
    default: return ILLEGAL;   // C.FLDSP/C.FSDSP 属于 D 扩展
    }
}

} // namespace rvc


