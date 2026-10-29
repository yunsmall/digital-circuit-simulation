// RiscVCPU — RISC-V 软核处理器，Wishbone 总线主设备
// 支持 RV32IMA + S/U/M 特权级 + Sv32 MMU
#pragma once
#include <cstdint>
#include <type_traits>
#include <string>
#include "dcs/Component.h"

namespace dsc {

namespace RV {
    // ---- Opcode ----
    constexpr uint32_t OP_IMM = 0x13, LUI = 0x37, AUIPC = 0x17, OP = 0x33, OP_32 = 0x3B;
    constexpr uint32_t JAL = 0x6F, JALR = 0x67, BRANCH = 0x63, LOAD = 0x03;
    constexpr uint32_t STORE = 0x23, MISC_MEM = 0x0F, SYSTEM = 0x73, AMO = 0x2F;
    constexpr uint32_t LOAD_FP = 0x07, STORE_FP = 0x27, OP_FP = 0x53;

    // ---- OP-FP 的 funct7 ----
    constexpr uint32_t F7_FADD=0x00, F7_FSUB=0x04, F7_FMUL=0x08, F7_FDIV=0x0C;
    constexpr uint32_t F7_FSQRT=0x2C, F7_FSGNJ=0x10, F7_FMINMAX=0x14;
    constexpr uint32_t F7_FCMP=0x50, F7_FCVT_W_S=0x60, F7_FCVT_S_W=0x68;
    constexpr uint32_t F7_FMV_X_W=0x70, F7_FMV_W_X=0x78;

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

    // ---- 计数器 / 环境配置 / 只读标识 ----
    // 这些原来直接以字面量散落在 switch 里，和上面成对的命名混用，既难对照规范也容易写错
    constexpr uint32_t CSR_MCYCLE=0xB00, CSR_MINSTRET=0xB02;
    constexpr uint32_t CSR_MCYCLEH=0xB80, CSR_MINSTRETH=0xB82;
    constexpr uint32_t CSR_CYCLE=0xC00, CSR_TIME=0xC01, CSR_INSTRET=0xC02;
    constexpr uint32_t CSR_CYCLEH=0xC80, CSR_TIMEH=0xC81, CSR_INSTRETH=0xC82;
    constexpr uint32_t CSR_MCOUNTEREN=0x306, CSR_SCOUNTEREN=0x106, CSR_MSTATUSH=0x310;
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
    constexpr uint32_t SATP_MODE=1u<<31; // Sv32

    // ---- 位域 ----
    constexpr int SH_OP=0, SH_RD=7, SH_F3=12, SH_RS1=15, SH_RS2=20, SH_F7=25;
    constexpr uint32_t M_OP=0x7F, M_RD=0x1F, M_F3=0x7, M_RS1=0x1F, M_RS2=0x1F, M_F7=0x7F, M_F12=0xFFF;

    // ---- 页表项位 ----
    constexpr uint32_t PTE_V=1u<<0, PTE_R=1u<<1, PTE_W=1u<<2, PTE_X=1u<<3;
    constexpr uint32_t PTE_U=1u<<4, PTE_G=1u<<5, PTE_A=1u<<6, PTE_D=1u<<7;
    constexpr uint32_t PPN_MASK=0x3FFFFFu, PAGE_OFF_MASK=0xFFFu;

    // ---- TLB ----
    constexpr int TLB_SIZE = 32;
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
    bool halted() const;
    uint32_t freg(int idx) const { return (idx >= 0 && idx < 32) ? _state.fregs[idx] : 0; }
    uint32_t fcsr() const { return _state.fcsr; }
    uint32_t mcause() const { return _state.mcause; } // 仅供调试/测试判断 trap 原因
    uint32_t mepc() const { return _state.mepc; }
    uint32_t mtval() const { return _state.mtval; }

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
        // MMU_AD_GAP 是 A/D 写回前的空拍：必须先拉低 stb 结束读 PTE 那笔事务，
        // 再重新发起写事务，不能靠"事务中途把 we 翻成 1"——不是所有从设备都认这个语义
        // GAP 是"结束上一笔事务的空拍"：真实硬件完成一次读事务（ack）后必须先拉低 stb，
        // 再重新发起写事务，不能在事务中途翻转 we——那不是 Wishbone 的语义
        AMO_GAP, AMO_WRITE, MMU_WALK_L1, WAIT_MMU_L1, MMU_WALK_L2, WAIT_MMU_L2, MMU_AD_GAP, MMU_AD_WRITE
    };

    struct CpuState {
        XLen regs[32], pc;
        // 64 位计数器：RV32 上 cycle/instret 由低高两个 CSR 拼成，硬件内部必须按 64 位递增
        uint64_t cycles, instret;
        uint32_t fregs[32]; // 浮点寄存器按 IEEE754 位模式存放，避免来回转换
        uint32_t fcsr;      // frm[7:5] | fflags[4:0]
        Phase phase;
        uint32_t inst;
        bool prev_clk;

        // 特权级
        uint8_t priv; // 当前特权级 (0=U, 1=S, 3=M)

        // M-mode CSR
        uint32_t mstatus, mtvec, mepc, mcause, mie, mip, mscratch, mtval;
        // 决定 S 态能否读 cycle/time/instret（U 态还要再过 scounteren，本核未实现 S 态计数器委派）
        uint32_t mcounteren;
        uint32_t scounteren;
        // PMP 配置与地址：本核不做保护，但必须能原样存回。
        // 软件靠"写 0 再读回验证"判断 PMP 是否已关闭，恒返回 0 会让它误判
        uint32_t pmpcfg[4], pmpaddr[16];
        uint32_t medeleg, mideleg;

        // S-mode CSR
        uint32_t sstatus, stvec, sepc, scause, stval, sie, sip, sscratch, satp;

        // Sv32 TLB
        // ASID 在 RV32 是 satp[30:22] 共 9 位，用 uint8_t 会截断高位，导致 TLB 命中错误页表项
        struct TlbEntry { uint32_t vpn, ppn; uint16_t asid; uint8_t flags; bool valid, global; };
        TlbEntry tlb[RV::TLB_SIZE];
        uint32_t tlb_cycle; // 用于 LRU 替换

        // L1 PTE 缓存（1 项，覆盖 4MB）
        uint32_t l1_cache_tag;   // vaddr[31:22]
        uint32_t l1_cache_ppn;   // L1 PTE >> 10
        bool l1_cache_valid;

        // MMU 遍历上下文（TLB miss 时保存，遍历完成后重试原指令）
        uint32_t mmu_vaddr;      // 虚地址
        bool mmu_is_write;
        bool mmu_is_exec;
        XLen mmu_root;           // satp 根页表物理地址（Sv32 就有 34 位，32 位存不下）
        XLen mmu_l1_addr;        // L1 PTE 总线地址
        XLen mmu_l2_addr;        // L2 PTE 总线地址

        // LR/SC 保留地址
        uint32_t lr_addr;
        bool lr_valid;

        // 总线上下文
        uint32_t bus_adr, bus_wdat;
        bool bus_we, bus_amo;
        uint8_t bus_sel, bus_funct3, bus_rd, bus_amo_f5;
        uint8_t bus_fp; // 访存结果写浮点寄存器（FLW）而不是整数寄存器
        bool wfi_halted; // 已执行 WFI，等待中断唤醒
        uint32_t fetch_pa; // 取指经过 MMU 翻译后的物理地址，总线重发时要用它而不是 pc
        uint8_t mmu_priv;  // 本次页表遍历使用的特权级（MPRV 时可能不同于当前特权级）
        uint32_t mmu_pte;      // 需要写回页表的 PTE（补 A/D 位）
        uint32_t mmu_pte_addr; // 该 PTE 的物理地址
        bool mmu_pte_is_l1;    // 写回的是 L1（大页）还是 L2 PTE
        // 指令缓存：直接映射，每行一条指令。循环体是最常见的取指热点，
        // 缓存下来可以完全跳过总线事务。
        struct ICacheLine { uint32_t tag; uint32_t inst; bool valid; };
        ICacheLine icache[256];
        uint32_t ic_hits, ic_misses;

        // 非对齐访存拆成两次对齐事务时的中间状态。真实从设备只接受字对齐地址 +
        // 字节使能，跨字访问必须由主设备自己拆开，不能改总线语义去迁就 CPU。
        XLen acc_va;        // 原始（可能未对齐）物理地址
        // 存两次事务的拼接结果：SD 要 64 位，32 位会让 >>32 成为未定义行为
        uint64_t acc_raw;   // 第一次读回的字；写操作时是待写数据
        uint8_t acc_f3;     // funct3，决定访问宽度
        uint8_t acc_stage;  // 0=单次事务, 1=第二次事务已发起, 2=第二次事务完成
    };

    CpuState _state;
    bool _prepared = false;

    static void cpuTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs);
    static void cpuReset(void *state_ptr);
    static void execute(CpuState &s, uint32_t inst,
                        XLen &wb_adr, uint32_t &wb_dat, bool &wb_we,
                        uint8_t &wb_sel, bool &wb_stb, bool &wb_cyc,
                        bool &need_mem);
    static XLen csrRead(const CpuState &s, uint32_t addr);
    static void csrWrite(CpuState &s, uint32_t addr, XLen val);
    static bool takeInterrupt(CpuState &s, XLen &trap_cause);
    static bool takeTrap(CpuState &s, XLen cause, XLen tval);
    static bool mmuTranslate(CpuState &s, XLen vaddr, XLen &paddr,
                             bool is_write, bool is_exec);
    // 返回 0 表示放行，否则返回应触发的页错误 cause。
    // TLB 命中、L1 大页、L2 4KB 页三条路径共用，避免改一处漏两处。
    static uint32_t checkPerms(uint32_t pte, bool is_write, bool is_exec, uint8_t priv, uint32_t mstatus);
};

// 实现全在 .cpp 里并显式实例化，这里只抑制隐式实例化，
// 免得每个包含者都去编译上千行模板代码
extern template class RiscVCPU<32>;
extern template class RiscVCPU<64>;

} // namespace dsc
