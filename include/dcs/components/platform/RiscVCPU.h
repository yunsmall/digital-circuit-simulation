// RiscVCPU — RISC-V 软核处理器，Wishbone 总线主设备
// 支持 RV32IMA + S/U/M 特权级 + Sv32 MMU
#pragma once
#include <cstdint>
#include <type_traits>
#include <string>
#include "dcs/Component.h"
#include "dcs/util/Int128.h"

// 每拍都要调用的函数不需要栈保护：序言/尾声里的 canary 读写实测占到采样周期的一小半
#if defined(__clang__) || defined(__GNUC__)
#define DCS_NO_STACK_PROTECTOR __attribute__((no_stack_protector))
#else
#define DCS_NO_STACK_PROTECTOR
#endif

// 每拍调用一次、参数多、序言重的函数需要强制内联。
// MSVC 用 __forceinline，GCC/Clang 用 always_inline 属性
#if defined(_MSC_VER) && !defined(__clang__)
#define DCS_FORCE_INLINE __forceinline
#else
#define DCS_FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace dsc {

namespace RV {
    // ---- Opcode ----
    constexpr uint32_t OP_IMM = 0x13, LUI = 0x37, AUIPC = 0x17, OP = 0x33;
    // RV64 的两组 32 位运算：0x1B 是 I 型（addiw 等，imm[11:5] 不是 funct7），
    // 0x3B 是 R 型（addw 等，funct7 才有意义）。混用会把立即数高位当 funct7 判非法
    constexpr uint32_t OP_IMM_32 = 0x1B, OP_32 = 0x3B;
    constexpr uint32_t JAL = 0x6F, JALR = 0x67, BRANCH = 0x63, LOAD = 0x03;
    constexpr uint32_t STORE = 0x23, MISC_MEM = 0x0F, SYSTEM = 0x73, AMO = 0x2F;
    constexpr uint32_t LOAD_FP = 0x07, STORE_FP = 0x27, OP_FP = 0x53;

    // ---- OP-FP 的 funct7 ----
    constexpr uint32_t F7_FADD=0x00, F7_FSUB=0x04, F7_FMUL=0x08, F7_FDIV=0x0C;
    constexpr uint32_t F7_FSQRT=0x2C, F7_FSGNJ=0x10, F7_FMINMAX=0x14;
    constexpr uint32_t F7_FCMP=0x50, F7_FCVT_W_S=0x60, F7_FCVT_S_W=0x68;
    constexpr uint32_t F7_FMV_X_W=0x70, F7_FMV_W_X=0x78;
    // D 扩展的 funct7 就是 F 的 | 1（fmt=01），所以判断双精度看 f7 & 1，
    // 取基础 funct7 用 f7 & ~1。这样 D 的分支可以和 F 一一对应，不用再排一张表
    constexpr uint32_t FMT_D = 0x01;

    // ---- Z 扩展的编码常量 ----
    // 这些扩展复用 OP/OP-IMM，靠 funct7 区分。规范里同一功能的寄存器形式和立即数
    // 形式常常用不同的 funct7（bset 是 0x14 而 bclr 是 0x24），逐个命名免得代码里
    // 出现一串看不出含义的裸数字
    constexpr uint32_t F7_ZBA_SHADD  = 0x10;   // sh1add/sh2add/sh3add 及其 .uw
    constexpr uint32_t F7_ZBB_MINMAX = 0x05;   // max/maxu/min/minu
    constexpr uint32_t F7_ZBB_ANDN   = 0x20;   // andn/orn/xnor（与 SUB/SRA 共用）
    constexpr uint32_t F7_ZBB_ROL    = 0x30;   // rol/ror/rolw/rorw
    constexpr uint32_t F7_ZBB_CNT    = 0x30;   // clz/ctz/cpop/sext.b/sext.h（OP-IMM）
    constexpr uint32_t F7_ZBB_ORCB   = 0x14;   // orc.b（OP-IMM）
    constexpr uint32_t F7_ZBB_REV8   = 0x34;   // rev8/brev8
    constexpr uint32_t F7_ZBS_BSET   = 0x14;   // bset/bseti
    constexpr uint32_t F7_ZBS_BCLR   = 0x24;   // bclr/bclri/bext/bexti
    constexpr uint32_t F7_ZBS_BINV   = 0x34;   // binv/binvi
    constexpr uint32_t F7_ZBC_CLMUL  = 0x05;   // clmul/clmulh/clmulr
    constexpr uint32_t F7_ZICOND     = 0x07;   // czero.eqz/czero.nez
    constexpr uint32_t F7_ZUW        = 0x04;   // add.uw/pack/packh/zext.h
    constexpr uint32_t F7_ZUSLLI     = 0x02;   // slli.uw
    // 用 rs2 域区分的子操作码
    constexpr uint32_t ZSUB_CLZ=0, ZSUB_CTZ=1, ZSUB_CPOP=2, ZSUB_SEXT_B=4, ZSUB_SEXT_H=5;
    constexpr uint32_t ZSUB_ORCB=7, ZSUB_BREV8=7, ZSUB_REV8=0x18;

    // ---- 浮点 CSR ----
    constexpr uint32_t CSR_FFLAGS=0x001, CSR_FRM=0x002, CSR_FCSR=0x003;

    // ---- Funct3 ----
    // mstatus 里限制 S 态行为的三个位（写进去就必须真正生效，否则等于骗软件）
    constexpr uint32_t MSTATUS_TVM = 1u << 20, MSTATUS_TW = 1u << 21, MSTATUS_TSR = 1u << 22;
    constexpr uint32_t F3_ADDI=0, F3_SLLI=1, F3_SLTI=2, F3_SLTIU=3, F3_XORI=4, F3_SRLI=5, F3_ORI=6, F3_ANDI=7;
    constexpr uint32_t F3_ADD=0, F3_SLL=1, F3_SLT=2, F3_SLTU=3, F3_XOR=4, F3_SRL=5, F3_OR=6, F3_AND=7;
    constexpr uint32_t F3_BEQ=0, F3_BNE=1, F3_BLT=4, F3_BGE=5, F3_BLTU=6, F3_BGEU=7;
    constexpr uint32_t F3_LB=0, F3_LH=1, F3_LW=2, F3_LBU=4, F3_LHU=5;
    constexpr uint32_t F3_SB=0, F3_SH=1, F3_SW=2;
    constexpr uint32_t F3_CSRRW=1, F3_CSRRS=2, F3_CSRRC=3, F3_CSRRWI=5, F3_CSRRSI=6, F3_CSRRCI=7;

    // ---- Funct7 ----
    constexpr uint32_t F7_NORMAL=0x00, F7_SUB_SRA=0x20;

    // ---- Funct5 (AMO) ----
    constexpr uint32_t F5_LR=0x02, F5_SC=0x03, F5_AMOSWAP=0x01, F5_AMOADD=0x00;
    constexpr uint32_t F5_AMOXOR=0x04, F5_AMOAND=0x0C, F5_AMOOR=0x08;
    constexpr uint32_t F5_AMOMIN=0x10, F5_AMOMAX=0x14, F5_AMOMINU=0x18, F5_AMOMAXU=0x1C;

    // ---- Funct12 ----
    constexpr uint32_t F12_ECALL=0x000, F12_EBREAK=0x001, F12_MRET=0x302, F12_SRET=0x102, F12_WFI=0x105;

    // ---- M-mode CSR ----
    constexpr uint32_t CSR_MSTATUS=0x300, CSR_MISA=0x301, CSR_MEDELEG=0x302, CSR_MIDELEG=0x303;
    constexpr uint32_t CSR_MIE=0x304, CSR_MTVEC=0x305, CSR_MSCRATCH=0x340;
    constexpr uint32_t CSR_MEPC=0x341, CSR_MCAUSE=0x342, CSR_MTVAL=0x343, CSR_MIP=0x344;

    // ---- S-mode CSR ----
    constexpr uint32_t CSR_SSTATUS=0x100, CSR_SIE=0x104, CSR_STVEC=0x105;
    constexpr uint32_t CSR_SSCRATCH=0x140, CSR_SEPC=0x141, CSR_SCAUSE=0x142, CSR_STVAL=0x143;
    constexpr uint32_t CSR_SIP=0x144, CSR_SATP=0x180;

    // ---- Debug 触发模块（Sdtrig 的可选部分）----
    // 官方 breakpoint 用例要的就是这几个 CSR + mcontrol(type=2)
    constexpr uint32_t CSR_TSELECT=0x7A0, CSR_TDATA1=0x7A1, CSR_TDATA2=0x7A2;
    constexpr uint32_t CSR_TDATA3=0x7A3, CSR_TINFO=0x7A4, CSR_TCONTROL=0x7A5;
    constexpr int TRIGGER_NUM = 4;         // 触发槽位数
    constexpr uint32_t TRIG_TYPE_MCONTROL = 2;
    constexpr uint32_t TRIG_M = 1u << 6;   // mcontrol.m：匹配 M 态访存
    constexpr uint32_t TRIG_EXECUTE = 1u << 2, TRIG_STORE = 1u << 1, TRIG_LOAD = 1u << 0;

    // ---- 计数器 / 环境配置 / 只读标识 ----
    // 这些原来直接以字面量散落在 switch 里，和上面成对的命名混用，既难对照规范也容易写错
    constexpr uint32_t CSR_MCYCLE=0xB00, CSR_MINSTRET=0xB02;
    constexpr uint32_t CSR_MCYCLEH=0xB80, CSR_MINSTRETH=0xB82;
    constexpr uint32_t CSR_CYCLE=0xC00, CSR_TIME=0xC01, CSR_INSTRET=0xC02;
    constexpr uint32_t CSR_CYCLEH=0xC80, CSR_TIMEH=0xC81, CSR_INSTRETH=0xC82;
    constexpr uint32_t CSR_MCOUNTEREN=0x306, CSR_SCOUNTEREN=0x106, CSR_MSTATUSH=0x310;
    // Smrnmi 的 mnstatus：本核不支持可恢复 NMI，但寄存器必须存在——
    // 启动代码会无条件写它（riscv-tests 的 init 里就是 csrwi mnstatus, 8），
    // 缺失会被当成非法指令，整个测试直接跑飞
    constexpr uint32_t CSR_MNSTATUS=0x744;
    constexpr uint32_t CSR_MVENDORID=0xF11, CSR_MARCHID=0xF12, CSR_MIMPID=0xF13, CSR_MHARTID=0xF14;

    // ---- PMP ----
    // pmpcfg 四项、pmpaddr 十六项。RV64 的 pmpcfg 每项 8 字节，只用偶数号
    constexpr uint32_t CSR_PMPCFG0=0x3A0, CSR_PMPCFG3=0x3A3;
    constexpr uint32_t CSR_PMPADDR0=0x3B0, CSR_PMPADDR15=0x3BF;

    // ---- mstatus 位 ----
    constexpr uint32_t MSTATUS_SIE=1u<<1, MSTATUS_MIE=1u<<3, MSTATUS_SPIE=1u<<5;
    constexpr uint32_t MSTATUS_MPIE=1u<<7, MSTATUS_SPP=1u<<8, MSTATUS_MPP=3u<<11;
    constexpr uint32_t MSTATUS_SUM=1u<<18; // 置位后 S 态才允许 load/store 访问 U 页
    constexpr uint32_t MSTATUS_MXR=1u<<19; // 置位后 load 可读执行页（R=0,X=1）
    constexpr uint32_t MSTATUS_MPRV=1u<<17; // 置位后 M 态数据访存按 MPP 的特权级翻译

    // ---- 中断位 ----
    constexpr uint32_t IRQ_SSI=1u<<1, IRQ_MSI=1u<<3, IRQ_STI=1u<<5, IRQ_MTI=1u<<7;
    constexpr uint32_t IRQ_SEI=1u<<9, IRQ_MEI=1u<<11;

    // ---- mcause ----
    // 中断位是 XLEN 的最高位：RV32 在 bit31、RV64 在 bit63，由调用方按 XLEN 取
    constexpr uint32_t MCAUSE_IRQ=1u<<31;
    constexpr uint32_t EXC_IAM=0, EXC_IAF=1, EXC_II=2, EXC_BP=3, EXC_LAM=4, EXC_LAF=5;
    constexpr uint32_t EXC_SAM=6, EXC_SAF=7, EXC_ECALL_U=8, EXC_ECALL_S=9, EXC_ECALL_M=11;
    constexpr uint32_t EXC_IPF=12, EXC_LPF=13, EXC_SPF=15;

    // ---- 特权级 ----
    constexpr uint8_t PRIV_U=0, PRIV_S=1, PRIV_M=3;

    // ---- satp ----
    constexpr uint32_t SATP_MODE=1u<<31; // Sv32：RV32 的 satp[31]
    constexpr uint32_t SATP_MODE_SV39 = 8; // Sv39：RV64 的 satp[63:60]

    // ---- 位域 ----
    constexpr int SH_OP=0, SH_RD=7, SH_F3=12, SH_RS1=15, SH_RS2=20, SH_F7=25;
    constexpr uint32_t M_OP=0x7F, M_RD=0x1F, M_F3=0x7, M_RS1=0x1F, M_RS2=0x1F, M_F7=0x7F, M_F12=0xFFF;

    // ---- 页表项位 ----
    // Sv32 与 Sv39 的 PTE 低位布局相同，区别在 PPN 宽度和 PTE 本身的字节数
    constexpr uint32_t PTE_V=1u<<0, PTE_R=1u<<1, PTE_W=1u<<2, PTE_X=1u<<3;
    constexpr uint32_t PTE_U=1u<<4, PTE_G=1u<<5, PTE_A=1u<<6, PTE_D=1u<<7;
    constexpr uint32_t PPN_MASK=0x3FFFFFu, PAGE_OFF_MASK=0xFFFu;
    // PTE 中 PPN 的位宽：Sv32 22 位，Sv39 44 位
    constexpr uint64_t PPN_MASK_SV39 = (1ull << 44) - 1;

    // ---- SBI ----
    // 扩展号放在 a7、功能号放在 a6（SBI 1.0 的约定）
    constexpr uint64_t SBI_EXT_SET_TIMER        = 0x00;       // legacy
    constexpr uint64_t SBI_EXT_CONSOLE_PUTCHAR  = 0x01;       // legacy
    constexpr uint64_t SBI_EXT_SHUTDOWN         = 0x08;       // legacy
    constexpr uint64_t SBI_EXT_BASE             = 0x10;
    constexpr uint64_t SBI_EXT_TIME             = 0x54494D45; // "TIME"
    constexpr uint64_t SBI_EXT_SRST             = 0x53525354; // "SRST"
    constexpr uint64_t SBI_FID_SET_TIMER        = 0;
    // spec version 的编码是 (major << 24) | minor
    constexpr uint64_t SBI_SPEC_VERSION_1_0     = 0x01000000;
    constexpr uint64_t SBI_FID_GET_SPEC_VERSION = 0;
    constexpr uint64_t SBI_FID_GET_IMPL_ID      = 1;
    constexpr uint64_t SBI_FID_GET_IMPL_VERSION = 2;
    constexpr uint64_t SBI_FID_PROBE_EXTENSION  = 3;
    constexpr uint64_t SBI_FID_SRST_RESET       = 0;

    // ---- TLB ----
    // 2 路组相联：每次翻译最多两次比较。之前是全相联 32 项线性扫描，
    // 在"每条指令都要翻译一次取指地址"的热路径上白烧几十次比较
    constexpr int TLB_WAYS = 2;
    constexpr int TLB_SIZE = 32;
    constexpr int TLB_SETS = TLB_SIZE / TLB_WAYS;
} // namespace RV

// 只有 32 和 64 两种位宽有意义（RV32/RV64），传别的值编译期直接拒绝
template<int N>
concept SupportedXlen = (N == 32 || N == 64);

// 放在命名空间级才能用在模板参数表和作用域里；类内再起个别名 XLen 方便书写
template<int N> requires SupportedXlen<N>
using XLenOf = std::conditional_t<N == 32, uint32_t, uint64_t>;

template<int XLEN> requires SupportedXlen<XLEN>
class RiscVCPU : public Component {
public:
    // 地址与通用寄存器的宽度：RV32 用 32 位，RV64 用 64 位
    using XLen = std::conditional_t<XLEN == 32, uint32_t, uint64_t>;
    // 有符号形式，用于比较/符号扩展；在 32 位构建里仍旧是 int32_t，行为不变
    using SXLen = std::conditional_t<XLEN == 32, int32_t, int64_t>;

    explicit RiscVCPU(const std::string &name);
    ~RiscVCPU() override;

    // halted：执行 WFI 期间为高，有中断待处理时自动唤醒
    XLen reg(int idx) const;
    XLen pc() const;
    // 复位后的 PC 是 0；测试镜像若链接在 DRAM_BASE 这类高地址，由外部在复位后
    // 把入口地址写进来，否则 PC 相对寻址会按低地址算错
    void setPc(XLen v) { _state.pc = v; }
    // 启动 Linux 时用：RISC-V 启动协议要求 a0=hartid、a1=DTB 物理地址。
    // 和 setPc 一样直接改状态，必须在 init() 之后、第一次 tick 之前调用
    void setReg(int idx, XLen v) {
        if (idx > 0 && idx < 32)
            _state.regs[idx] = v;
    }
    bool halted() const;
    uint32_t freg(int idx) const { return (idx >= 0 && idx < 32) ? _state.fregs[idx] : 0; }
    // 双精度寄存器是 64 位的，freg() 只给低 32 位，浮点测试要用完整位模式
    uint64_t freg64(int idx) const { return (idx >= 0 && idx < 32) ? _state.fregs[idx] : 0; }
    uint32_t fcsr() const { return _state.fcsr; }
    // 取指缓存命中/缺失计数，供性能分析用
    uint64_t icacheHits() const { return _state.ic_hits; }
    uint64_t icacheMisses() const { return _state.ic_misses; }
    // 最近 8 次取指缺失的地址，供性能分析
    const uint32_t *icacheRecentMisses() const { return _state.ic_miss_pc; }
    uint32_t mcause() const { return _state.mcause; } // 仅供调试/测试判断 trap 原因
    uint32_t mepc() const { return _state.mepc; }
    uint32_t mtval() const { return _state.mtval; }
    // 当前特权级与 mstatus，供调试/测试核对 trap 语义
    uint8_t priv() const { return _state.priv; }
    uint32_t mstatus() const { return _state.mstatus; }
    // 最近执行过的 64 条指令地址，按时间顺序返回（最早的在前）。
    // 内核查不出问题时靠它看 PC 在哪些函数间打转
    std::vector<uint32_t> pcTrace() const {
        std::vector<uint32_t> out;
        out.reserve(CpuState::PC_TRACE);
        // 计数器必须 64 位：用 uint8_t 时跑满 256 的整数倍（长时间仿真很容易撞上）
        // 会绕回 0，pcTrace() 就返回空数组
        const uint64_t n =
                _state.pc_trace_idx < (uint64_t) CpuState::PC_TRACE ? _state.pc_trace_idx : (uint64_t) CpuState::PC_TRACE;
        const uint64_t start = _state.pc_trace_idx - n;
        for (uint64_t i = 0; i < n; ++i)
            out.push_back(_state.pc_trace[(start + i) & (CpuState::PC_TRACE - 1)]);
        return out;
    }

    bool hasCombinational() const override { return false; }
    bool hasSequential() const override { return true; }
    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override;
    std::string genResetCode() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    enum class Phase : uint8_t {
        FETCH, WAIT_FETCH, EXECUTE, WAIT_MEM, HALTED,
        // C 扩展下，落在字内偏移 2 的 32 位指令跨两个字，要再读一个字来拼
        FETCH_HI_GAP, WAIT_FETCH_HI,  // 跨字取指：两次读之间要拉低 stb 一拍
        // GAP 是"结束上一笔事务的空拍"：真实硬件完成一次读事务（ack）后必须先拉低 stb，
        // 再重新发起写事务，不能在事务中途翻转 we——那不是 Wishbone 的语义
        AMO_GAP, AMO_WRITE,
        // 页表遍历：Sv32 的 PTE 4 字节一次读完；Sv39 的 PTE 8 字节，
        // 总线只有 32 位，要拆成低半、高半两笔事务（LO/HI 各有一拍拉低 stb 的发起相位）
        MMU_WALK_LO, WAIT_MMU_LO, MMU_WALK_HI, WAIT_MMU_HI,
        // RV64 的 .D 原子操作要读写各两段（总线只有 32 位）：
        // 读完低半再读高半、写低半再写高半，中间都隔一个 GAP 结束事务
        AMO_READ_HI, AMO_GAP_HI, AMO_WRITE_HI
    };

    struct CpuState {
        XLen regs[32], pc;
        // 64 位计数器：RV32 上 cycle/instret 由低高两个 CSR 拼成，硬件内部必须按 64 位递增
        uint64_t cycles, instret;
        bool instret_written;   // 写 minstret 的那条指令不递增，好让紧接着的读取拿到写入值
        // 浮点寄存器按 IEEE754 位模式存放，避免来回转换。D 扩展要 64 位，
        // F 指令只读写低 32 位
        uint64_t fregs[32];
        uint32_t fcsr;      // frm[7:5] | fflags[4:0]
        Phase phase;
        uint32_t inst;
        // 指令字节数：C 扩展下一条指令可能是 2 字节。pc 增量和 JAL/JALR 的返回地址
        // 都要用它，不能写死 +4
        uint8_t inst_len;
        bool prev_clk;

        // 特权级
        uint8_t priv; // 当前特权级 (0=U, 1=S, 3=M)

        // M-mode CSR
        // mstatus 必须是 XLEN 宽：RV64 的 SXL/UXL 在 bit33:32、bit35:34，SD 在 bit63。
        // 用 uint32_t 存的话每次 csr_write(sstatus, ...) 都会把高位掩掉，
        // __switch_to 保存/恢复 sstatus 之后 FS/SD 状态就乱了。
        // mie/mip 的中断位都在低 12 位，保持 32 位
        XLen mstatus;
        uint32_t mie, mip;
        // mscratch/sscratch 存的是内核栈虚拟地址，必须是 XLEN 宽，否则
        // 0xFFFFFFFF801xxxxx 会被截成 0x801xxxxx 的物理地址，trap 入口立刻写飞
        XLen mscratch;
        XLen mtvec, mepc, mcause, mtval;
        // 决定 S 态能否读 cycle/time/instret（U 态还要再过 scounteren，本核未实现 S 态计数器委派）
        uint32_t mcounteren;
        uint32_t scounteren;
        // PMP 配置与地址：本核不做保护，但必须能原样存回。
        // 软件靠"写 0 再读回验证"判断 PMP 是否已关闭，恒返回 0 会让它误判
        uint32_t pmpcfg[4], pmpaddr[16];
        uint32_t medeleg, mideleg;

        // S-mode CSR
        uint32_t sstatus, sie, sip;
        XLen sscratch;
        XLen stvec, sepc, scause, stval, satp;

        // Debug 触发模块：tselect 选槽位，tdata1 存 mcontrol 配置（含 type），
        // tdata2 存匹配地址。tcontrol.mte 置位后 M 态触发才生效
        uint32_t tselect;
        XLen tdata1[RV::TRIGGER_NUM], tdata2[RV::TRIGGER_NUM], tdata3[RV::TRIGGER_NUM];
        uint32_t tcontrol;

        // Sv32/Sv39 TLB
        // ASID 在 RV32 是 satp[30:22] 共 9 位，RV64 是 satp[59:44] 共 16 位，
        // 用 uint8_t 会截断高位，导致 TLB 命中错误页表项
        // vpn 在 Sv39 下是 31 位（39-12+4），ppn 取 32 位物理地址的低 20 位，都能装进 uint32_t
        struct TlbEntry { uint32_t vpn, ppn; uint16_t asid; uint8_t flags; bool valid, global; };
        TlbEntry tlb[RV::TLB_SIZE];
        // 每个组的替换指针：0 表示下一项换 way0，1 表示换 way1
        uint8_t tlb_lru[RV::TLB_SETS];

        // 顶层 PTE 缓存（1 项）：Sv32 覆盖 4MB、Sv39 覆盖 1GB。
        // 存的是最高级非叶子 PTE 的 PPN，命中后可直接跳过最高一级总线读
        uint64_t l1_cache_tag;   // 虚拟地址的 VPN[最高级]
        uint64_t l1_cache_ppn;   // 该 PTE 的 PPN（Sv39 是 44 位，必须用 64 位存）
        bool l1_cache_valid;

        // MMU 遍历上下文（TLB miss 时保存，遍历完成后重试原指令）
        uint64_t mmu_vaddr;      // 虚地址：Sv39 是 39 位，32 位装不下
        bool mmu_is_write;
        // 页表遍历要跨好几拍，这期间取指也会调 mmuTranslate 把 mmu_is_write 清掉；
        bool mmu_pte_is_write;
        // A/D 检查必须看开始遍历时的那个副本
        bool mmu_is_exec;
        uint64_t mmu_next_page;  // 下一级页表基址（当前 PTE 的 PPN << 12）
        uint64_t mmu_pte_addr;   // 当前 PTE 的总线地址（低半）
        uint64_t mmu_pte_lo;     // Sv39 已读回的低 32 位，等高半拼成完整 PTE
        uint8_t mmu_level;       // 当前遍历到第几级：Sv32 从 1 到 0，Sv39 从 2 到 0

        // LR/SC 保留地址
        uint32_t lr_addr;
        bool lr_valid;

        // 总线上下文
        uint32_t bus_adr, bus_wdat;
        bool bus_we, bus_amo;
        uint8_t bus_sel, bus_funct3, bus_rd, bus_amo_f5;
        // 原子操作的 64 位拆分：总线只有 32 位，.D 要分两段读、两段写
        uint32_t amo_lo;      // 读阶段先到的低半
        uint32_t amo_hi;      // 写阶段要写的高半
        uint64_t amo_rs2;     // 完整的 rs2：bus_wdat 只有 32 位，.D 必须单独存
        bool amo_is_d;        // 本次原子操作是 .D
        bool amo_hi_done;     // .D 的高半已经读过
        bool amo_hi_write;    // 当前这笔写是 .D 的高半
        uint8_t bus_fp; // 访存结果写浮点寄存器（FLW）而不是整数寄存器
        // 最近 64 条已执行指令的 PC（环形）。内核查不出问题时，
        // 把这段打出来就能看出它到底在哪些函数之间打转。
        // 只在 PC 变化时记：连续相同的 PC 看不出任何东西，还白白每拍写内存
        static constexpr int PC_TRACE = 64;
        uint32_t pc_trace[PC_TRACE];
        uint64_t pc_trace_idx;
        uint64_t pc_trace_last;
        // 这一拍要摆到总线上的三组值。C++ 侧只写这三个字段，由 JIT 存根按固定偏移
        // 取出来用立即数地址写线网：省掉每根输出线一次指针解引用，也让存根能就着
        // 寄存器比较"和上一拍一样就整组跳过"
        uint32_t out_adr, out_wdat;
        uint8_t out_flags; // bit0 we，bit1..4 sel，bit5 stb，bit6 cyc，bit7 halted

        bool wfi_halted; // 已执行 WFI，等待中断唤醒
        // SBI 关机（SHUTDOWN/SRST）后必须永久停机：内核关机后若被中断唤醒会
        // 继续跑后面的指令，所以它和 WFI 的"可唤醒停机"要分开记
        bool powered_off;
        // SBI set_timer 的到期时刻（以 cycles 计）。CLINT 的 MTI 是另一条
        // 定时器来源，两者都映射到 STIP，见 cpuTick 里的合并逻辑
        uint64_t timer_deadline;
        uint32_t fetch_pa; // 取指经过 MMU 翻译后的物理地址，总线重发时要用它而不是 pc
        uint32_t fetch_word0; // 跨字 32 位指令拼接时暂存第一个字
        XLen fetch_hi_addr;   // 跨字取指时第二个字的地址，显式保存避免重算
        uint8_t fetch_gap_cnt;  // 跨字取指拉低 stb 之后，ack 撤销还要再等一拍
        uint8_t mmu_priv;  // 本次页表遍历使用的特权级（MPRV 时可能不同于当前特权级）
        // 指令缓存：直接映射，每行一条指令。循环体是最常见的取指热点，
        // 缓存下来可以完全跳过总线事务。
        // 一个 4 字节字里可能装两条压缩指令（pc&3 为 0 和 2），
        // 所以每条缓存行同时存两个位置解压后的结果，命中时不用再解压
        struct ICacheLine {
            uint32_t tag;
            uint32_t raw;       // 原始 32 位字，32 位指令跨字时要用
            uint32_t inst[2];   // [0]=pc&3==0 处的指令，[1]=pc&3==2 处的指令
            uint8_t len[2];     // 对应的指令字节数
            bool valid;
        };
        ICacheLine icache[256];
        uint32_t ic_hits, ic_misses;
        uint32_t ic_miss_pc[8];  // 最近 8 次取指缺失的地址（环形），调试用
        uint32_t ic_miss_seq;

        // 非对齐访存拆成两次对齐事务时的中间状态。真实从设备只接受字对齐地址 +
        // 字节使能，跨字访问必须由主设备自己拆开，不能改总线语义去迁就 CPU。
        XLen acc_va;        // 原始（可能未对齐）物理地址
        // 存两次事务的拼接结果：SD 要 64 位，32 位会让 >>32 成为未定义行为
        uint64_t acc_raw;   // 第一次读回的字；写操作时是待写数据
        // 读路径的字节累加器：按段依次左移 32 位拼进来，最后一次性右移 va&3。
        // 这样 1/2/3 段走同一套公式，不用为每种对齐各写一遍移位。
        // 用 U128 而不是 __int128：MSVC 没有后者，见 dcs/util/Int128.h
        U128 acc_wide;
        uint8_t acc_seg;    // 已经收进来几段
        // 8 字节非对齐访问跨 3 个总线字，第二段的数据要单独存一份才能拼出完整值
        uint32_t acc_raw2;
        uint8_t acc_f3;     // funct3，决定访问宽度
        uint8_t acc_stage;  // 0=单次事务, 1=第二次事务已发起, 2=第二次事务完成
    };

    CpuState _state;
    bool _prepared = false;

    static void cpuTick(void *state_ptr, const uint8_t *const *inputs)
            DCS_NO_STACK_PROTECTOR;
    static void cpuReset(void *state_ptr);
    static DCS_FORCE_INLINE void execute(CpuState &s, uint32_t inst,
                        XLen &wb_adr, uint32_t &wb_dat, bool &wb_we,
                        uint8_t &wb_sel, bool &wb_stb, bool &wb_cyc,
                        bool &need_mem);
    static XLen csrRead(const CpuState &s, uint32_t addr);
    static void csrWrite(CpuState &s, uint32_t addr, XLen val);
    static bool takeInterrupt(CpuState &s, XLen &trap_cause);
    static bool takeTrap(CpuState &s, XLen cause, XLen tval);
    static bool mmuTranslate(CpuState &s, XLen vaddr, XLen &paddr,
                             bool is_write, bool is_exec);
    // 检查是否有 mcontrol 触发槽命中（地址 + 操作类型 + 特权级 + tcontrol.mte）
    static bool triggerMatch(const CpuState &s, XLen addr, uint32_t op_bit);
    // 处理一个刚读回的页表项：校验合法性/权限/A/D，装填 TLB 或推进到下一级。
    // 返回 true 表示 TLB 已装填（调用方把相位设回取指/执行）；false 表示
    // 已 trap 或者还要继续遍历，相位已经在函数里设好
    static bool handlePte(CpuState &s, uint64_t pte);
    // 返回 0 表示放行，否则返回应触发的页错误 cause。
    // TLB 命中、各级页表叶子路径共用，避免改一处漏两处。
    static uint32_t checkPerms(uint32_t pte, bool is_write, bool is_exec, uint8_t priv, uint32_t mstatus);
};

// 实现全在 .cpp 里并显式实例化，这里只抑制隐式实例化，
// 免得每个包含者都去编译上千行模板代码
extern template class RiscVCPU<32>;
extern template class RiscVCPU<64>;

} // namespace dsc
