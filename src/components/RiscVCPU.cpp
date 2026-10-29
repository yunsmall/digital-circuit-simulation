#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstdio>
// RiscVCPU — RISC-V 软核 (RV32IMA + S/U/M + Sv32 MMU)
#include "dcs/components/RiscVCPU.h"
#include "dcs/util/RvcDecode.h"
#include <cstring>
#include <format>

namespace dsc {

// satp 的 mode 域位置随 XLEN 变：RV32 在 bit31、Rv64 在 bit63:60。
// 分页是否开启要看整个 mode 域，不能只测一个位——RV64 下 bit31 落在 ASID 里
template<int XLEN> requires SupportedXlen<XLEN>
static inline bool pagingEnabled(unsigned long long satp) {
    if constexpr (XLEN == 64) return (satp >> 60) != 0;
    else return (satp >> 31) != 0;
}

// Sv32 是两级、每级 10 位 VPN，PTE 4 字节；Sv39 是三级、每级 9 位 VPN，PTE 8 字节
template<int XLEN> struct SvParams {
    static constexpr int levels   = (XLEN == 32) ? 2 : 3;
    static constexpr int vpn_bits = (XLEN == 32) ? 10 : 9;
    static constexpr int pte_size = (XLEN == 32) ? 4 : 8;
    static constexpr uint64_t vpn_field_mask = (1ull << vpn_bits) - 1;
    // 根页表 PPN 的位置：Sv32 是 satp[21:0]，Sv39 是 satp[43:0]
    static constexpr uint64_t root_ppn_mask = (XLEN == 32) ? 0x3FFFFFULL : ((1ull << 44) - 1);
    static constexpr uint32_t asid(uint64_t satp) {
        if constexpr (XLEN == 64) return (uint32_t)((satp >> 44) & 0xFFFF);
        else return (uint32_t)((satp >> 22) & 0x1FF);
    }
};

constexpr int SLOT = 16;
constexpr int I_CLK = 0, I_RST = 1, I_DAT = 2, I_ACK = 3, I_IRQ = 4, I_MTI = 5, I_MSI = 6, I_SEIP = 7;
constexpr int O_ADR = 0, O_WDAT = 1, O_WE = 2, O_SEL = 3, O_STB = 4, O_CYC = 5, O_HALT = 6, O_COUNT = 7;

// ============================================================
template<int XLEN> requires SupportedXlen<XLEN>
RiscVCPU<XLEN>::RiscVCPU(const std::string &name) : Component(name, "riscv_cpu") {
    addInput("clk", 1); addInput("rst", 1); addInput("wb_dat_i", 32);
    addInput("wb_ack_i", 1); addInput("irq", 1);
    addInput("mti", 1); addInput("msi", 1);
    addInput("seip", 1); // S 级外部中断，通常接 PLIC 的 S-mode context
    addOutput("wb_adr_o", 32, false, true); addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_we_o", 1, false, true); addOutput("wb_sel_o", 4, false, true);
    addOutput("wb_stb_o", 1, false, true); addOutput("wb_cyc_o", 1, false, true);
    addOutput("halted", 1, false, true);
    std::memset(&_state, 0, sizeof(_state));
    _state.phase = Phase::FETCH;
    _state.priv = RV::PRIV_M;
}
template<int XLEN> requires SupportedXlen<XLEN>
RiscVCPU<XLEN>::~RiscVCPU() = default;
template<int XLEN> requires SupportedXlen<XLEN>
XLenOf<XLEN> RiscVCPU<XLEN>::reg(int idx) const { return (idx>=0&&idx<=31)?_state.regs[idx]:0; }
template<int XLEN> requires SupportedXlen<XLEN>
XLenOf<XLEN> RiscVCPU<XLEN>::pc() const { return _state.pc; }
template<int XLEN> requires SupportedXlen<XLEN>
bool RiscVCPU<XLEN>::halted() const { return _state.wfi_halted; }
template<int XLEN> requires SupportedXlen<XLEN>
bool RiscVCPU<XLEN>::prepare(std::string &) { _prepared = true; return true; }
template<int XLEN> requires SupportedXlen<XLEN>
std::vector<Component::JitSymbol> RiscVCPU<XLEN>::extraJitSymbols() const {
    // 符号名必须随 XLEN 区分：RV32 和 RV64 的 cpuTick 是两份不同的函数，
    // 同名注册会让后注册的覆盖先注册的，另一档实例就会被喂进布局不匹配的 CpuState
    const std::string sfx = (XLEN == 32) ? "_32" : "_64";
    return {{"_dcs_cpu_tick" + sfx, reinterpret_cast<void*>(&cpuTick)},
            {"_dcs_cpu_reset" + sfx, reinterpret_cast<void*>(&cpuReset)}};
}

template<int XLEN> requires SupportedXlen<XLEN>
std::string RiscVCPU<XLEN>::genStateDecl() const {
    return std::format("static void *_cpu_state_{0} = (void *)0x{1:X}ULL;\n",
                       _id, reinterpret_cast<uintptr_t>(&_state));
}
template<int XLEN> requires SupportedXlen<XLEN>
std::string RiscVCPU<XLEN>::genInitCode() const  { return std::format("    _dcs_cpu_reset{1}(_cpu_state_{0});\n", _id, (XLEN == 32) ? "_32" : "_64"); }
template<int XLEN> requires SupportedXlen<XLEN>
std::string RiscVCPU<XLEN>::genDeinitCode() const { return ""; }
template<int XLEN> requires SupportedXlen<XLEN>
std::string RiscVCPU<XLEN>::genResetCode() const { return std::format("    _dcs_cpu_reset{1}(_cpu_state_{0});\n", _id, (XLEN == 32) ? "_32" : "_64"); }

template<int XLEN> requires SupportedXlen<XLEN>
std::string RiscVCPU<XLEN>::genFuncDef_seq() const {
    int id = _id;
    std::string code;
    code += std::format("// RISC-V CPU: {}\n", name());
    const std::string sfx = (XLEN == 32) ? "_32" : "_64";
    code += "extern void _dcs_cpu_tick" + sfx + "(void *, const uint8_t *const *, uint8_t *const *);\n";
    code += "extern void _dcs_cpu_reset" + sfx + "(void *);\n\n";
    code += std::format("static void {}() {{\n", funcName_seq());
    code += genInputPtrArray("_in");
    // 输出与组合元件一样经 genOutputWrite：它同时写 _c_out 与 _w，
    // 避免本元件比别处多出一拍传播延迟
    code += std::format("    uint8_t _obuf[{}][{}];\n", O_COUNT, SLOT);
    code += "    dcs_memset(_obuf, 0, sizeof(_obuf));\n";
    code += std::format("    uint8_t *_op[{}] = {{", O_COUNT);
    for (int i = 0; i < O_COUNT; i++) code += std::format("{}_obuf[{}]", i ? "," : "", i);
    code += "};\n";
    code += std::format("    _dcs_cpu_tick{1}(_cpu_state_{0}, _in, _op);\n", id, sfx);
    const int ow[O_COUNT] = {32, 32, 1, 4, 1, 1, 1};
    for (int i = 0; i < O_COUNT; i++)
        code += std::format("    {{ uint32_t _v=0; dcs_memcpy(&_v,_obuf[{}],4); {} }}\n", i,
                            genOutputWrite(i, "_v", ow[i]));
    code += "}\n";
    return code;
}
template<int XLEN> requires SupportedXlen<XLEN>
std::unique_ptr<Component> RiscVCPU<XLEN>::clone(const std::string &n) const {
    // 与其余元件一致：连运行状态一起复制，复制出来的核可以直接接着跑
    auto p = std::make_unique<RiscVCPU>(n);
    p->_state = _state;
    return p;
}

// ============================================================
namespace {
struct CpuIn {
    bool clk, rst, ack, irq, mti, msi, seip; uint32_t dat;
    explicit CpuIn(const uint8_t *const *in) {
        // 固定 4 字节的 memcpy 会被编成单条 mov，比手写字节拼接更快，别改成移位
        clk=(in[I_CLK][0]&1)!=0; rst=(in[I_RST][0]&1)!=0;
        std::memcpy(&dat,in[I_DAT],4); ack=(in[I_ACK][0]&1)!=0;
        irq=(in[I_IRQ][0]&1)!=0; mti=(in[I_MTI][0]&1)!=0; msi=(in[I_MSI][0]&1)!=0;
        seip=(in[I_SEIP][0]&1)!=0;
    }
};
void packOut(uint8_t *const *out, uint32_t adr, uint32_t wdat, bool we, uint8_t sel,
             bool stb, bool cyc, bool halted) {
    std::memcpy(out[O_ADR],&adr,4); std::memcpy(out[O_WDAT],&wdat,4);
    out[O_WE][0]=we?1:0; out[O_SEL][0]=sel;
    out[O_STB][0]=stb?1:0; out[O_CYC][0]=cyc?1:0;
    out[O_HALT][0]=halted?1:0;
}
}

// ============================================================
template<int XLEN> requires SupportedXlen<XLEN>
void RiscVCPU<XLEN>::cpuTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs) {
    auto &s = *static_cast<CpuState*>(state_ptr);
    CpuIn in(inputs);
    if (in.rst) [[unlikely]] { cpuReset(state_ptr); packOut(outputs,0,0,false,0,false,false,false); return; }
    // 外部中断线每 tick 反映到 mip，否则停机期间收不到中断。mti 同时驱动 MTIP 和
    // 委托后的 STIP：M 态若占着 MTIP 不放，需要软件自己关 MTIE 才轮得到 S 态
    if (in.irq) s.mip|=RV::IRQ_MEI; else s.mip&=~RV::IRQ_MEI;
    if (in.mti) s.mip|=RV::IRQ_MTI; else s.mip&=~RV::IRQ_MTI;
    // 定时器中断委托给 S 态时（mideleg.STIP=1），硬件把 MTI 反映成 STIP：
    // 只声明 mideleg.STIP 可写却不产生 STIP，会让等待 S 态定时器中断的软件永久挂起
    if ((s.mideleg & RV::IRQ_STI) && in.mti) s.mip |= RV::IRQ_STI; else s.mip &= ~RV::IRQ_STI;
    if (in.msi) s.mip|=RV::IRQ_MSI; else s.mip&=~RV::IRQ_MSI;
    if (in.seip) s.mip|=RV::IRQ_SEI; else s.mip&=~RV::IRQ_SEI;
    // 与 WFI 的停机条件保持一致：只有 mie 里本地使能的挂起中断才唤醒，
    // 否则会在"唤醒 - 发现无法处理 - 重新停机"之间反复抖动。
    // 这里刻意不看 mstatus.MIE：全局关闭时也要退出 WFI，由后续中断判定决定去留
    if (s.wfi_halted && (s.mip & s.mie)) {
        s.wfi_halted = false;
        s.phase = Phase::FETCH; // 唤醒后从暂停的 PC 继续取指
    }

    bool rising = !s.prev_clk && in.clk; s.prev_clk = in.clk;
    if (rising) s.cycles++; // cycle 按时钟上升沿计数，与真实硬件的 mcycle 一致

    if (!rising) {
        XLen adr=0; uint32_t wdat=0; bool we=false,stb=false,cyc=false; uint8_t sel=0;
        switch (s.phase) {
        // FETCH 这一拍地址要等上升沿翻译完才能定，不能先按 pc 发请求，
        // 否则从设备先把 pc 当地址做掉一次事务，页表读取会拿到取指数据
        case Phase::FETCH: adr=s.pc; sel=0x0F; stb=cyc=false; break;
        case Phase::WAIT_FETCH: adr=s.fetch_pa & ~(XLen)3; sel=0x0F; stb=cyc=true; break;
        // 跨字 32 位指令要读下一个字；漏这个 case 会让这一拍 stb/cyc 全为 0，
        // 从设备不响应，取指永远等不到 ack
        case Phase::FETCH_HI_GAP: adr=s.fetch_hi_addr; sel=0x0F; stb=false; cyc=true; break;
        case Phase::WAIT_FETCH_HI: adr=s.fetch_hi_addr; sel=0x0F; stb=cyc=true; break;
        case Phase::WAIT_MEM: {
            // 与上升沿分支保持一致：拆分事务的拉低相位必须真的拉低 stb，
            // 否则同一时钟周期的另一半又把 stb 抬起来，从设备看不到事务边界。
            // stage 4 是"下一次上升沿才发起第二次事务"，这一拍必须继续拉低，
            // 否则会先用旧地址发一次假事务，从设备锁存旧地址后真正的事务再也不触发
            // 这些相位要求 stb 保持低电平：拆分事务的两次拉低，以及第三段等旧 ack 撤销。
            // 漏掉 7 的话下降沿会把 stb 又抬起来，从设备永远看不到事务结束
            // 两拍拉低的相位都要在这里标出来：上升沿推进相位之后，下降沿会再读一次
            // acc_stage，漏掉就会把下半拍的 stb 又抬回去，从设备看不到事务边界
            bool hold_off = (s.acc_stage >= 2 && s.acc_stage <= 4)
                         || (s.acc_stage == 7) || (s.acc_stage == 8);
            adr=s.bus_adr; wdat=s.bus_wdat; we=s.bus_we; sel=s.bus_sel;
            stb=cyc=!hold_off;
            break;
        }
        case Phase::WAIT_MMU_LO: adr=(XLen)s.mmu_pte_addr; sel=0x0F; stb=cyc=true; break;
        case Phase::WAIT_MMU_HI: adr=(XLen)(s.mmu_pte_addr + 4); sel=0x0F; stb=cyc=true; break;
        case Phase::MMU_WALK_LO: adr=(XLen)s.mmu_pte_addr; sel=0x0F; stb=cyc=true; break;
        case Phase::MMU_WALK_HI: adr=(XLen)(s.mmu_pte_addr + 4); sel=0x0F; stb=cyc=true; break;
        // RMW 空拍：cyc 保持高表示总线仍被本核占用（别的设备插不进来），
        // 只拉低 stb 让从设备看到事务边界——AMO 的原子性就靠这个
        case Phase::AMO_GAP: adr=s.bus_adr; wdat=s.bus_wdat; sel=0x0F; stb=false; cyc=true; break;
        case Phase::AMO_WRITE: adr=s.bus_adr; wdat=s.bus_wdat; we=true; sel=0x0F; stb=cyc=true; break;
        default: break;
        }
        packOut(outputs,adr,wdat,we,sel,stb,cyc,s.wfi_halted);
        return;
    }


    XLen wb_adr=0; uint32_t wb_dat=0; bool wb_we=false,wb_stb=false,wb_cyc=false; uint8_t wb_sel=0;

    switch (s.phase) {
    case Phase::FETCH: {
        // 取指同样过 MMU：S/U 态开启 satp 后 pc 是虚拟地址
        XLen fpa=0;
        if (!mmuTranslate(s, s.pc, fpa, false, true)) [[unlikely]] {
            if (s.phase == Phase::MMU_WALK_LO) {
                // mmuTranslate 起了页表遍历：本拍发起第一笔 PTE 读，立即转入等待
                wb_adr=(XLen)s.mmu_pte_addr; wb_sel=0x0F; wb_we=false; wb_stb=wb_cyc=true;
                s.phase = Phase::WAIT_MMU_LO;
                break;
            }
            // 页错误已进 trap，s.pc 现在是 trap 入口。M 态入口是物理地址可直接用；
            // S 态的 stvec 是虚拟地址，必须重新翻译，不能把它当物理地址取指。
            XLen fpa2=0;
            if (s.priv == RV::PRIV_M || !pagingEnabled<XLEN>(s.satp)) {
                s.fetch_pa = s.pc;
            } else if (!mmuTranslate(s, s.pc, fpa2, false, true)) {
                if (s.phase == Phase::MMU_WALK_LO) {
                    wb_adr=(XLen)s.mmu_pte_addr; wb_sel=0x0F; wb_we=false; wb_stb=wb_cyc=true;
                    s.phase = Phase::WAIT_MMU_LO;
                    break;
                }
                s.phase = Phase::FETCH; wb_stb = wb_cyc = false; // stvec 未映射：继续 trap，由软件负责
                break;
            } else {
                s.fetch_pa = fpa2;
            }
            wb_adr=s.fetch_pa; wb_sel=0x0F; wb_stb=wb_cyc=true; s.phase=Phase::WAIT_FETCH;
            break;
        }
        s.fetch_pa = fpa;
        // 取指断点：命中就不执行这条指令，直接进 breakpoint 陷阱（mepc = 当前 pc）
        if (triggerMatch(s, s.pc, RV::TRIG_EXECUTE)) [[unlikely]] {
            takeTrap(s, RV::EXC_BP, s.pc);
            s.phase = Phase::FETCH; wb_stb = wb_cyc = false;
            break;
        }
        {
            uint32_t ic_idx = (fpa >> 2) & 255u;
            uint32_t ic_tag = fpa >> 10;
            if (s.icache[ic_idx].valid && s.icache[ic_idx].tag == ic_tag) {
                // 命中时直接用缓存里解压好的指令：解压只在 miss 时做一次。
                // 一个 4 字节字里可能有两条压缩指令，所以按字内位置取对应那一份
                s.ic_hits++;
                const uint32_t half = (fpa & 2u) ? 1u : 0u;
                // len==0 表示这一半是跨字 32 位指令，缓存里装不下，退回总线去读
                if (s.icache[ic_idx].len[half] == 0) [[unlikely]] {
                    wb_adr = fpa & ~(XLen)3; wb_sel = 0x0F; wb_stb = wb_cyc = true;
                    s.phase = Phase::WAIT_FETCH;
                    break;
                }
                s.inst = s.icache[ic_idx].inst[half];
                s.inst_len = s.icache[ic_idx].len[half];
                s.phase = Phase::EXECUTE; // 命中就不再发起总线读
                wb_stb = wb_cyc = false;
                break;
            }
            s.ic_misses++;
        }
        // 总线只接受字对齐地址：压缩指令的 pc 可能是 4n+2，地址要抹掉低两位，
        // 具体取哪个半字由 fetch_pa 的 bit1 在 WAIT_FETCH 里决定
        wb_adr=fpa & ~(XLen)3; wb_sel=0x0F; wb_stb=wb_cyc=true; s.phase=Phase::WAIT_FETCH;
        break;
    }

    case Phase::WAIT_FETCH:
        if (in.ack) {
            // 取指按字对齐读，压缩指令可能只占半个字：低 2 位不是 11 就是 16 位指令。
            // fetch_pa 的 bit1 决定取低半还是高半
            const uint32_t lo16 = in.dat & 0xFFFFu;
            const uint32_t hi16 = in.dat >> 16;
            const uint32_t half = (s.fetch_pa & 2u) ? hi16 : lo16;
            if ((half & 3u) == 3u) {
                if (s.fetch_pa & 2u) {
                    // 32 位指令落在字内偏移 2：它跨了两个字，必须再读下一个字拼起来。
                    // 这一路不进 icache（一行只存一个字的两个半字位置，装不下跨字指令）
                    s.fetch_word0 = in.dat;
                    s.fetch_hi_addr = (s.fetch_pa & ~(XLen)3) + 4;
                    s.fetch_gap_cnt = 0;
                    // 先把 stb 拉低结束这一笔事务，下一拍才发起读下一个字。
                    // 直接跳 WAIT_FETCH_HI 的话 stb 一直保持高，从设备看不到事务边界，
                    // 不会给第二个 ack
                    wb_stb = false; wb_cyc = true;
                    s.phase = Phase::FETCH_HI_GAP;
                    break;
                }
                s.inst = in.dat;
                s.inst_len = 4;
            } else {
                s.inst = rvc::expand<XLEN>((uint16_t)(half & 0xFFFFu));
                s.inst_len = 2;
                if (s.inst == rvc::ILLEGAL) [[unlikely]] {
                    // 未实现或非法的压缩编码要按非法指令处理，不能拿 0 继续执行
                    s.inst = 0;
                    takeTrap(s, RV::EXC_II, 0);
                    wb_stb = wb_cyc = false; s.phase = Phase::FETCH;
                    break;
                }
            }
            // 把两个字内位置都解压好存进缓存：同一行可能先后被 pc&3==0 和 ==2 访问
            uint32_t ic_idx = (s.fetch_pa >> 2) & 255u;
            typename CpuState::ICacheLine &line = s.icache[ic_idx];
            line.tag = s.fetch_pa >> 10;
            line.raw = in.dat;
            line.inst[0] = ((lo16 & 3u) == 3u) ? in.dat : rvc::expand<XLEN>((uint16_t)lo16);
            line.len[0]  = ((lo16 & 3u) == 3u) ? 4 : 2;
            line.inst[1] = ((hi16 & 3u) == 3u) ? 0u : rvc::expand<XLEN>((uint16_t)hi16);
            line.len[1]  = ((hi16 & 3u) == 3u) ? 0u : 2;   // len=0 表示"这一半要跨字，别用缓存"
            line.valid = true;
            wb_stb=wb_cyc=false; s.phase=Phase::EXECUTE;
        }
        else { wb_adr=s.fetch_pa & ~(XLen)3; wb_sel=0x0F; wb_stb=wb_cyc=true; }
        break;

    case Phase::FETCH_HI_GAP:
        // 拉低 stb 后，从设备要先看到 stb 撤销才清 ack_pending，主设备再晚一拍才看到
        // ack 变低；ack 变低之后从设备的 prev_stb 也才归 0。少等这一拍，第二笔的
        // stb 上升沿会被从设备当成上一笔的延续，不会重新锁存地址
        if (!in.ack) {
            if (s.fetch_gap_cnt >= 1) {
                wb_adr = s.fetch_hi_addr; wb_sel = 0x0F;
                wb_stb = wb_cyc = true;
                s.phase = Phase::WAIT_FETCH_HI;
            } else {
                s.fetch_gap_cnt++;
                wb_stb = false; wb_cyc = true;
            }
        } else {
            wb_stb = false; wb_cyc = true;
        }
        break;

    case Phase::WAIT_FETCH_HI:
        if (in.ack) {
            // 跨字 32 位指令：低半来自第一个字的高 16 位，高半来自新读到的字
            s.inst = (s.fetch_word0 >> 16) | (in.dat << 16);
            s.inst_len = 4;
            wb_stb = wb_cyc = false;
            s.phase = Phase::EXECUTE;
        } else {
            wb_adr = (s.fetch_pa & ~(XLen)3) + 4; wb_sel = 0x0F; wb_stb = wb_cyc = true;
        }
        break;

    case Phase::EXECUTE: {
        XLen trap_cause=0;
        if (takeInterrupt(s, trap_cause)) [[unlikely]] {
            takeTrap(s, trap_cause, 0);
            s.phase=Phase::FETCH; wb_sel=0x0F; wb_stb=wb_cyc=false;
            break;
        }
        bool need_mem=false;
        execute(s, s.inst, wb_adr, wb_dat, wb_we, wb_sel, wb_stb, wb_cyc, need_mem);
        if (s.phase == Phase::HALTED) {
            // WFI 停机这一拍不能再拉高 stb：那会向从设备发一次虚假取指，
            // 有状态的从设备（如缓存填充）可能就此卡在半完成
            wb_adr = 0; wb_dat = 0; wb_we = false; wb_sel = 0;
            wb_stb = wb_cyc = false;
        }
        else if (s.phase == Phase::MMU_WALK_LO) {
            // mmuTranslate 起了页表遍历，输出第一次 PTE 读请求并立即转为等待状态
            wb_adr = (XLen)s.mmu_pte_addr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
            s.phase = Phase::WAIT_MMU_LO;
        }
        else if (need_mem) [[unlikely]] { s.bus_adr=wb_adr; s.bus_wdat=wb_dat; s.bus_we=wb_we; s.bus_sel=wb_sel; s.phase=Phase::WAIT_MEM; }
        else { s.phase=Phase::FETCH; wb_sel=0x0F; wb_stb=wb_cyc=false; }
        break;
    }

    case Phase::WAIT_MEM:
        // 两次事务之间的相位：主从都是时序元件，_w 传递有一拍延迟，
        // 只拉低一拍 stb 不足以让从设备忘记上一次 ack，必须占满这些拍再重新发起。
        // 第三段：8 字节非对齐访问要跨 3 个总线字（sd x, 1(base) 要写 1..8），
        // 前两段只覆盖 7 字节。7=拉低 8=发起 9=等 ack 10=已到达
        // 第三段：8 字节非对齐访问跨 3 个总线字（ld/sd 偏移 1..3），前两段只覆盖 7 字节。
        // 地址必须在拉低那一拍就放上总线：从设备晚一拍才采到地址，等到发起那一拍
        // 才改地址，它锁存的仍是上一段的字
        if (s.acc_stage >= 7 && s.acc_stage <= 10) [[unlikely]] {
            const XLen a3 = (s.acc_va & ~(XLen)3) + 8;
            if (s.acc_stage == 7 || s.acc_stage == 8) {
                uint32_t dat = 0;
                uint8_t sel = 0x0F;
                if (s.bus_we) {
                    const uint32_t p = (uint32_t)(s.acc_va & 3);
                    const uint32_t n = (s.acc_f3 == 3) ? 8 : 4;
                    const uint32_t rest = p + n - 8;   // 第三段要写的字节数
                    // 按 p 定位：p=1 取最高字节，p=2 取高两字节，p=3 取高三字节
                    dat = (uint32_t)(s.acc_raw >> (8 * (8 - p)));
                    sel = (uint8_t)((1u << rest) - 1u);
                }
                wb_adr = a3; wb_dat = dat; wb_we = s.bus_we; wb_sel = sel;
                wb_stb = false; wb_cyc = true;
                s.bus_adr = a3; s.bus_wdat = dat; s.bus_sel = sel;
                if (s.acc_stage == 8 && !in.ack) s.acc_stage = 9;   // 旧 ack 撤了才发起
                else if (s.acc_stage == 7) s.acc_stage = 8;
                break;
            }
            // 发起第三段：地址已经就位，这里只拉高 stb
            if (s.acc_stage == 9) {
                wb_adr = s.bus_adr; wb_dat = s.bus_wdat; wb_we = s.bus_we; wb_sel = s.bus_sel;
                wb_stb = wb_cyc = true;
                s.acc_stage = 10;
                break;
            }
            // stage 10 落到下面的统一 ack 处理：这一拍 ack 高就是第三段的数据
        }
        if (s.acc_stage >= 2 && s.acc_stage <= 5) [[unlikely]] {
            if (s.acc_stage <= 3) { // 拉低 stb，等从设备看到事务结束
                wb_adr = s.bus_adr; wb_dat = s.bus_wdat; wb_we = s.bus_we; wb_sel = s.bus_sel;
                wb_stb = wb_cyc = false;
                s.acc_stage++;
                break;
            }
            if (s.acc_stage == 4) { // 发起第二次事务
                uint32_t p = s.acc_va & 3;
                XLen a2 = (s.acc_va & ~(XLen)3) + 4;
                uint32_t dat = 0;
                uint8_t sel = 0x0F;
                if (s.bus_we) {
                    uint32_t n = (s.acc_f3==RV::F3_SB)?1:(s.acc_f3==RV::F3_SH)?2:(s.acc_f3==3)?8:4;
                    uint32_t first = 4 - p;
                    uint32_t rest = n - first;
                    dat = (uint32_t)(s.acc_raw >> (8 * first));
                    sel = (uint8_t)((1u << rest) - 1u);
                }
                wb_adr = a2; wb_dat = dat; wb_we = s.bus_we; wb_sel = sel; wb_stb = wb_cyc = true;
                s.bus_adr = a2; s.bus_wdat = dat; s.bus_sel = sel;
                s.acc_stage = 5;
                break;
            }
            // stage 5：保持 stb，但这一拍看到的 ack 还是上一次事务的残留，跳过
            wb_adr = s.bus_adr; wb_dat = s.bus_wdat; wb_we = s.bus_we; wb_sel = s.bus_sel;
            wb_stb = wb_cyc = true;
            s.acc_stage = 6;
            break;
        }
        wb_adr=s.bus_adr; wb_dat=s.bus_wdat; wb_we=s.bus_we; wb_sel=s.bus_sel; wb_stb=wb_cyc=true;
        if (in.ack) {
            if (!s.bus_we) {
                // 读响应
                uint32_t raw=in.dat;
                if (s.bus_amo) [[unlikely]] {
                    // .D 要先把 8 字节读齐：总线只有 32 位，读完低半再读高半
                    if (s.amo_is_d && !s.amo_hi_done) {
                        s.amo_lo = raw;
                        s.amo_hi_done = true;
                        XLen hi_adr = (s.bus_adr & ~(XLen)7) + 4;
                        // 必须先把 stb 拉低：一直保持高电平，从设备看不到事务边界，
                        // 不会给新的 ack
                        wb_adr = hi_adr; wb_dat = 0; wb_we = false; wb_sel = 0x0F;
                        wb_stb = wb_cyc = false;
                        s.bus_adr = hi_adr;
                        s.phase = Phase::AMO_READ_HI;
                        break;
                    }

                    // 完整的旧值和 rs2：.D 用 64 位，.W 只取低 32 位
                    const uint64_t old_val = s.amo_is_d
                        ? (((uint64_t) raw << 32) | (uint64_t) s.amo_lo)
                        : (uint64_t) raw;
                    const uint64_t rs2_64 = s.amo_is_d ? s.amo_rs2 : (uint64_t) s.bus_wdat;
                    const uint32_t f5 = s.bus_amo_f5;
                    const uint32_t lo_old = (uint32_t) old_val, lo_rs2 = (uint32_t) rs2_64;

                    // 先算出新值；LR 和失败的 SC 不需要写回，用 need_write 统一收尾
                    uint64_t new_val = 0;
                    bool need_write = true;
                    if (f5 == RV::F5_LR) {
                        need_write = false;
                        // .D 读完高半后 bus_adr 停在 +4，保留地址要按 8 字节对齐存，
                        // 否则随后的 SC 拿原始地址来比会永远不匹配
                        s.lr_addr = s.amo_is_d ? (s.bus_adr & ~(XLen)7) : s.bus_adr;
                        s.lr_valid = true;
                    } else if (f5 == RV::F5_SC) {
                        const XLen sc_adr = s.amo_is_d ? (s.bus_adr & ~(XLen)7) : s.bus_adr;
                        if (s.lr_valid && s.lr_addr == sc_adr) { s.lr_valid = false; new_val = rs2_64; }
                        else { s.lr_valid = false; need_write = false; }
                    } else if (s.amo_is_d) {
                        // .D 按 64 位算：.W 那套 32 位运算直接升宽会让 AMOMIN/MAX 的比较错位
                        switch (f5) {
                        case RV::F5_AMOSWAP: new_val = rs2_64; break;
                        case RV::F5_AMOADD:  new_val = old_val + rs2_64; break;
                        case RV::F5_AMOXOR:  new_val = old_val ^ rs2_64; break;
                        case RV::F5_AMOAND:  new_val = old_val & rs2_64; break;
                        case RV::F5_AMOOR:   new_val = old_val | rs2_64; break;
                        case RV::F5_AMOMIN:  new_val = ((int64_t) old_val < (int64_t) rs2_64) ? old_val : rs2_64; break;
                        case RV::F5_AMOMAX:  new_val = ((int64_t) old_val > (int64_t) rs2_64) ? old_val : rs2_64; break;
                        case RV::F5_AMOMINU: new_val = (old_val < rs2_64) ? old_val : rs2_64; break;
                        case RV::F5_AMOMAXU: new_val = (old_val > rs2_64) ? old_val : rs2_64; break;
                        default: need_write = false; break;
                        }
                    } else {
                        uint32_t nv = 0;
                        switch (f5) {
                        case RV::F5_AMOSWAP: nv = lo_rs2; break;
                        case RV::F5_AMOADD:  nv = lo_old + lo_rs2; break;
                        case RV::F5_AMOXOR:  nv = lo_old ^ lo_rs2; break;
                        case RV::F5_AMOAND:  nv = lo_old & lo_rs2; break;
                        case RV::F5_AMOOR:   nv = lo_old | lo_rs2; break;
                        case RV::F5_AMOMIN:  nv = ((int32_t) lo_old < (int32_t) lo_rs2) ? lo_old : lo_rs2; break;
                        case RV::F5_AMOMAX:  nv = ((int32_t) lo_old > (int32_t) lo_rs2) ? lo_old : lo_rs2; break;
                        case RV::F5_AMOMINU: nv = (lo_old < lo_rs2) ? lo_old : lo_rs2; break;
                        case RV::F5_AMOMAXU: nv = (lo_old > lo_rs2) ? lo_old : lo_rs2; break;
                        default: need_write = false; break;
                        }
                        // .W 的结果要按 32 位截断后再符号/零扩展到 XLEN，见下面的写回
                        new_val = (uint64_t) nv;
                    }

                    // 单一收尾：LR/SC 的返回值、普通 AMO 的旧值、以及要不要写回都在这里决定。
                    // .W 的旧值符号扩展到 XLEN（与 LW 一致），.D 直接用完整 64 位
                    const XLen old_to_rd = s.amo_is_d ? (XLen) old_val : (XLen)(SXLen)(int32_t) lo_old;
                    // SC 成功返 0、失败返 1；LR 和普通 AMO 返回旧值
                    s.regs[s.bus_rd] = need_write
                        ? (f5 == RV::F5_SC ? (XLen) 0 : old_to_rd)
                        : (f5 == RV::F5_LR ? old_to_rd : (f5 == RV::F5_SC ? (XLen) 1 : 0));
                    if (need_write) {
                        if (s.amo_is_d) {
                            s.amo_hi = (uint32_t)(new_val >> 32);
                            s.bus_wdat = (uint32_t) new_val;
                            s.bus_adr &= ~(XLen)7;   // 读高半时地址停在 +4，退回低半
                        } else {
                            s.bus_wdat = (uint32_t) new_val;
                        }
                        s.bus_we = true;
                        s.phase = Phase::AMO_GAP;
                    } else {
                        s.bus_amo = false;
                        wb_dat = 0; wb_we = false; wb_sel = 0x0F; wb_stb = wb_cyc = false;
                        s.phase = Phase::FETCH;
                    }
                    break;
                }
                    // 普通读；跨字非对齐访问要再读一个字，与第一次读回的内容拼接
                    if (s.acc_stage == 1) {
                        s.acc_raw = raw;
                        s.acc_wide = (unsigned __int128) raw;   // 第一段落在最低 32 位
                        s.acc_seg = 1;
                        s.acc_stage = 2;      // 进入两次事务之间的拉低阶段
                        wb_adr = s.bus_adr; wb_dat=0; wb_we=false; wb_sel=0x0F;
                        wb_stb = wb_cyc = false;
                        break;
                    }
                    // 第二段刚到，如果 8 字节访问还差最后一段，先把这一段存下再去发第三段
                    if (s.acc_stage == 6) {
                        s.acc_wide |= (unsigned __int128) raw << 32;   // 第二段
                        s.acc_seg = 2;
                        const uint32_t pp = (uint32_t)(s.acc_va & 3);
                        const uint32_t nn = (s.acc_f3 == 3) ? 8 : 4;
                        if (pp + nn > 8) {
                            s.acc_raw2 = raw;
                            s.acc_stage = 7;   // 7 等旧 ack 撤销 -> 8 发起 -> 9 等新 ack -> 10 到达
                            break;
                        }
                    }
                    // 收尾：两段够用、或者三段都到齐了，都从这里统一取出结果。
                    // acc_wide 里每段的字节位置与内存一致，右移 va&3 就是目标起始字节。
                    // 对齐访存（acc_stage==0）走下面的 else，不碰 128 位运算——
                    // 那是热路径，非对齐本来就慢，代价要全部落在非对齐那一侧
                    XLen value;
                    if (s.acc_stage >= 10) {
                        s.acc_wide |= (unsigned __int128) raw << 64;   // 第三段
                        value = (XLen)(s.acc_wide >> (8 * (s.acc_va & 3)));
                        s.acc_stage = 0;
                    } else if (s.acc_stage >= 6) {
                        value = (XLen)(s.acc_wide >> (8 * (s.acc_va & 3)));
                        s.acc_stage = 0;
                    } else {
                        value = (XLen) raw >> (8 * (s.acc_va & 3));
                    }
                    // 窄加载要按 XLEN 符号扩展到整寄存器：RV64 下只扩到 32 位再赋值，
                    // 高 32 位会被零填充，负的字节/半字会变成大正数
                    XLen result=0;
                    switch (s.bus_funct3) {
                    case RV::F3_LB: result=(XLen)(SXLen)(int8_t)(value&0xFF); break;
                    case RV::F3_LH: result=(XLen)(SXLen)(int16_t)(value&0xFFFF); break;
                    case RV::F3_LW: result=(XLen)(SXLen)(int32_t)value; break;
                    case RV::F3_LBU: result=(XLen)(value&0xFF); break;
                    case RV::F3_LHU: result=(XLen)(value&0xFFFF); break;
                    case 3: // LD，只有 RV64 有
                        if constexpr (XLEN == 64) result = (XLen) value;
                        break;
                    case 6: // LWU：只取低 32 位并零扩展，非对齐时高半是相邻字
                        if constexpr (XLEN == 64) result = (XLen)(uint32_t) value;
                        break;
                    }
                    if (s.bus_fp) {
                        if (s.bus_funct3 == 3) {
                            // FLD 要 8 字节，而 value 是 XLen 宽——RV32 下只有 32 位，
                            // 高半会被截掉。直接从 acc_wide 取完整的 8 字节
                            s.fregs[s.bus_rd] = (uint64_t)(s.acc_wide >> (8 * (s.acc_va & 3)));
                        } else if (s.bus_funct3 == 1) {
                            // FLH：取低 16 位，高 48 位填全 1（NaN-boxing）
                            s.fregs[s.bus_rd] = 0xFFFFFFFFFFFF0000ULL | (uint64_t)(uint16_t) value;
                        } else {
                            // FLW 只提供 32 位，写回时高半必须填全 1（NaN-boxing）
                            s.fregs[s.bus_rd] = 0xFFFFFFFF00000000ULL | (uint64_t)(uint32_t) value;
                        }
                    } else s.regs[s.bus_rd]=result;
                    wb_dat=0; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=false; s.phase=Phase::FETCH;
            } else {
                // 写响应；跨字非对齐写在这里补写高出字的那几个字节
                if (!s.bus_amo && s.acc_stage == 1) { // AMO 的写确认不能被当成跨字写的第一段
                    s.acc_stage = 2;
                    wb_adr = s.bus_adr; wb_dat=0; wb_we=false; wb_sel=0x0F;
                    wb_stb = wb_cyc = false;
                    break;
                }
                if (s.bus_amo) {
                    s.bus_amo=false; // AMO 写完成，ld_val 已在 read 阶段写入 rd
                }
                // 第二段写完，8 字节非对齐访问还差最后一个字节，进第三段
                if (s.acc_stage == 6) {
                    const uint32_t pp = (uint32_t)(s.acc_va & 3);
                    const uint32_t nn = (s.acc_f3 == 3) ? 8 : 4;
                    if (pp + nn > 8) { s.acc_stage = 7; break; }
                }
                s.acc_stage = 0;
                wb_dat=0; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=false; s.phase=Phase::FETCH;
            }
        }
        break;

    case Phase::MMU_WALK_LO:
        // 发起当前级 PTE 低半读请求
        wb_adr = (XLen)s.mmu_pte_addr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        s.phase = Phase::WAIT_MMU_LO;
        break;

    case Phase::WAIT_MMU_LO:
        wb_adr = (XLen)s.mmu_pte_addr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        if (in.ack) {
            if constexpr (XLEN == 64) {
                // Sv39 的 PTE 是 8 字节：先存下低半，拉低 stb 结束这笔事务，
                // 下一拍 MMU_WALK_HI 再读高半。PTE 自然 8 字节对齐，两半不会跨页
                s.mmu_pte_lo = in.dat;
                s.phase = Phase::MMU_WALK_HI;
            } else {
                handlePte(s, in.dat);
            }
            wb_stb = wb_cyc = false;
        }
        break;

    case Phase::MMU_WALK_HI:
        wb_adr = (XLen)(s.mmu_pte_addr + 4); wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        s.phase = Phase::WAIT_MMU_HI;
        break;

    case Phase::WAIT_MMU_HI:
        wb_adr = (XLen)(s.mmu_pte_addr + 4); wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        if (in.ack) {
            handlePte(s, s.mmu_pte_lo | ((uint64_t)in.dat << 32));
            wb_stb = wb_cyc = false;
        }
        break;

    case Phase::AMO_GAP:
        // 这一拍同样要保持 cyc 高：它是 RMW 周期的中间拍，总线不能被别的设备拿走
        wb_adr = s.bus_adr; wb_dat = s.bus_wdat; wb_sel = 0x0F; wb_stb = false; wb_cyc = true;
        s.phase = Phase::AMO_WRITE;
        break;
    case Phase::AMO_WRITE:
        wb_adr = s.bus_adr; wb_dat = s.bus_wdat; wb_we = true; wb_sel = 0x0F; wb_stb = wb_cyc = true;
        if (in.ack) {
            // .D 的低半写完了，还要写高半。复用 AMO_GAP → AMO_WRITE 这条已验证的
            // 路径，只把地址和数据换成高半，比自己再造一套相位可靠
            if (s.amo_is_d && !s.amo_hi_write) {
                s.amo_hi_write = true;
                s.bus_adr = (s.bus_adr & ~(XLen)7) + 4;
                s.bus_wdat = s.amo_hi;
                wb_stb = false;
                s.phase = Phase::AMO_GAP;
                break;
            }
            // 与普通写完成后的收尾保持一致：清 AMO 标志、复位访问阶段、结束事务
            s.bus_amo = false;
            s.acc_stage = 0;
            wb_stb = wb_cyc = false;
            s.phase = Phase::FETCH;
        }
        break;
    // .D 的读高半：这一拍保持 stb 低，下一拍交给 WAIT_MEM 重新断言发起读
    case Phase::AMO_READ_HI:
        wb_adr = s.bus_adr; wb_dat = 0; wb_we = false; wb_sel = 0x0F;
        wb_stb = false; wb_cyc = true;
        s.phase = Phase::WAIT_MEM;
        break;

    case Phase::HALTED: break;
    }
    s.regs[0]=0;
    packOut(outputs, wb_adr, wb_dat, wb_we, wb_sel, wb_stb, wb_cyc, s.wfi_halted);
}

template<int XLEN> requires SupportedXlen<XLEN>
void RiscVCPU<XLEN>::cpuReset(void *state_ptr) {
    auto &s = *static_cast<CpuState*>(state_ptr);
    std::memset(&s,0,sizeof(s));
    s.phase=Phase::FETCH; s.priv=RV::PRIV_M;
    for (auto &l : s.icache) l.valid = false;
}

// ============================================================
// execute
// ============================================================
// csrImplemented 的定义在文件后半段，execute 需要先看到声明
template<int XLEN> requires SupportedXlen<XLEN>
static bool csrImplemented(uint32_t addr);

// ---- Zfh 半精度辅助 ----
// 不用 clang 的 _Float16：它依赖 compiler-rt 的 __extendhfsf2/__truncsfhf2，
// 本工程不链 compiler-rt，链接会直接失败。half 的字段少，按位展开即可，
// 中间结果一律用 double 承载：double 的 53 位尾数足以精确表示两个 half 的和/积，
// 所以"先算 double 再舍入到 half"不会引入双重舍入误差。
inline double halfToDouble(uint16_t h) {
    const bool neg = (h & 0x8000) != 0;
    const int e = (h >> 10) & 0x1F;
    const int m = h & 0x3FF;
    double v;
    if (e == 0) v = std::ldexp((double) m, -24);              // 非规格化：m * 2^-24
    else if (e == 31) v = (m == 0) ? std::numeric_limits<double>::infinity()
                                   : std::numeric_limits<double>::quiet_NaN();
    else v = std::ldexp((double)(0x400 | m), e - 25);        // 1.m * 2^(e-15)
    return neg ? -v : v;
}

// 按 RISC-V 的 rm 编码（0=RNE 1=RTZ 2=RDN 3=RUP 4=RMM）把 x 舍入成整数
inline double roundByRm(double x, uint32_t rm) {
    switch (rm) {
    case 0: {
        double fl = std::floor(x), ce = std::ceil(x);
        double dlo = x - fl, dhi = ce - x;
        if (dlo < dhi) return fl;
        if (dhi < dlo) return ce;
        return std::fmod(fl, 2.0) == 0.0 ? fl : ce;          // 中点取偶
    }
    case 1: return std::trunc(x);
    case 2: return std::floor(x);
    case 3: return std::ceil(x);
    default: { double t = std::trunc(x), f = x - t; if (std::fabs(f) >= 0.5) t += (x > 0 ? 1.0 : -1.0); return t; }
    }
}

// 把 double 按 rm 舍入成 half 位模式；fflags 用 RISC-V 的位布局累加（NX=1, OF=4, NV=0x10）
inline uint16_t doubleToHalf(double v, uint32_t rm, uint32_t &fflags) {
    const bool neg = std::signbit(v);
    const uint16_t sign = neg ? 0x8000 : 0;
    if (std::isnan(v)) {
        uint64_t bits; std::memcpy(&bits, &v, 8);
        // double 的 signaling NaN：无 quiet 位但有尾数
        const bool snan = (bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL
                       && (bits & 0x000FFFFFFFFFFFFFULL) != 0
                       && (bits & 0x0008000000000000ULL) == 0;
        if (snan) fflags |= 0x10;
        return 0x7E00;                                       // canonical NaN
    }
    if (std::isinf(v)) return sign | 0x7C00;
    if (v == 0.0) return sign;

    const double a = std::fabs(v);
    const double min_normal = std::ldexp(1.0, -14);
    const double max_half = 65504.0;                         // 2^16 - 2^5

    double ulp;
    int exp = 0;
    const bool subnormal = (a < min_normal);
    if (subnormal) {
        ulp = std::ldexp(1.0, -24);                          // 非规格化步长
    } else {
        int e2;
        std::frexp(a, &e2);                                  // a = f * 2^e2, f∈[0.5,1)
        exp = e2 - 1;
        ulp = std::ldexp(1.0, exp - 10);                     // 10 位尾数
    }

    // 溢出：|v| 超过 half 能表示的范围时按 rm 决定给 inf 还是最大有限值
    if (a > max_half) {
        const bool to_inf = (rm == 1) ? false                      // RTZ
                          : (rm == 2) ? neg                        // RDN
                          : (rm == 3) ? !neg                        // RUP
                          : (a >= 65520.0);                         // RNE/RMM：中点以上才到 inf
        if (to_inf) { fflags |= 0x04 | 0x01; return sign | 0x7C00; }
        fflags |= 0x04 | 0x01;
        return sign | 0x7BFF;                                     // 最大有限值
    }

    const double x = a / ulp;
    double n = roundByRm(x, rm);
    if (n != x) fflags |= 0x01;                                   // NX
    if (n == 0.0) return sign;

    uint16_t out;
    if (subnormal) {
        // 非规格化：n 就是尾数本身（n==1024 时自然进位到最小正规数的位模式）
        out = (uint16_t) n;
    } else {
        int e = exp + 15;
        int m = (int) n - 1024;                                  // 去掉隐含的 1
        if (m >= 1024) { e++; m = 0; }                           // 舍入进位
        out = (uint16_t)((e << 10) | m);
    }
    return sign | out;
}

template<int XLEN> requires SupportedXlen<XLEN>
void RiscVCPU<XLEN>::execute(CpuState &s, uint32_t inst,
                       XLenOf<XLEN> &wb_adr, uint32_t &wb_dat, bool &wb_we,
                       uint8_t &wb_sel, bool &wb_stb, bool &wb_cyc, bool &need_mem) {
    s.regs[0]=0; need_mem=false;
    uint32_t op=(inst>>RV::SH_OP)&RV::M_OP, rd=(inst>>RV::SH_RD)&RV::M_RD;
    uint32_t f3=(inst>>RV::SH_F3)&RV::M_F3, rs1=(inst>>RV::SH_RS1)&RV::M_RS1;
    uint32_t rs2=(inst>>RV::SH_RS2)&RV::M_RS2, f7=(inst>>RV::SH_F7)&RV::M_F7;
    int32_t ii=(int32_t)inst>>20, is=((int32_t)((inst>>7)&0x1F)|((int32_t)((inst>>25)&0x7F)<<5));
    is=(int32_t)((uint32_t) is<<20)>>20;
    int32_t ib=((int32_t)((inst>>8)&0xF)<<1)|((int32_t)((inst>>25)&0x3F)<<5)|
               ((int32_t)((inst>>7)&1)<<11)|((int32_t)((inst>>31)&1)<<12); ib=(int32_t)((uint32_t) ib<<19)>>19;
    int32_t iu=(int32_t)(inst&0xFFFFF000);
    int32_t ij=((int32_t)((inst>>21)&0x3FF)<<1)|((int32_t)((inst>>20)&1)<<11)|
               ((int32_t)((inst>>12)&0xFF)<<12)|((int32_t)((inst>>31)&1)<<20); ij=(int32_t)((uint32_t) ij<<11)>>11;
    // 这些量必须按 XLEN 取宽：RV64 下把 64 位寄存器截成 uint32_t 会让高半截直接丢失，
    // 依赖 64 位值的地址计算和跳转会算错
    XLen v1=s.regs[rs1], v2=s.regs[rs2];
    SXLen s1=(SXLen)v1, s2=(SXLen)v2;
    XLen npc=s.pc+s.inst_len;

        // 已知局限：这里假设宿主的浮点舍入方向是 RNE（默认值，本项目不调 fesetround）。
        // 若宿主被改成其他方向，"(t)float" 和 double 运算本身都会跟着变，
        // 彻底的做法是自带软浮点（如 SoftFloat），那是另一个量级的工程
        // C 的 double->float 转换永远是就近舍入，而 RISC-V 要求按 frm/rm 选模式。
        // 先取就近结果，再按方向用 nextafterf 修正一格。
        auto round_to_float = [&](double exact, int rmode, bool cancelled_to_zero = false) -> float {
            // 超出 float 范围时不能一律返回 inf：RTZ/RDN/RUP 对溢出的方向
            // 要返回最大有限值，即使得到 inf 也还要按模式翻转
            constexpr double kFmax = (double) std::numeric_limits<float>::max();
            if (std::isfinite(exact) && std::fabs(exact) > kFmax) {
                bool pos = exact > 0;
                switch (rmode) {
                case 1: return pos ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max(); // RTZ
                case 2: return pos ? std::numeric_limits<float>::max()
                                   : -std::numeric_limits<float>::infinity(); // RDN
                case 3: return pos ? std::numeric_limits<float>::infinity()
                                   : -std::numeric_limits<float>::max(); // RUP
                // RNE 和 RMM 都是"nearest"：中点是 Fmax 与 2^128 的中点。
                // Fmax = 2^128 - 2^104，所以中点是 2^128 - 2^103，不是 2^128
                case 4: // RMM
                default: // RNE
                    return std::fabs(exact) >= 0x1p128 - 0x1p103 ? (pos ? std::numeric_limits<float>::infinity()
                                                              : -std::numeric_limits<float>::infinity())
                                                       : (pos ? std::numeric_limits<float>::max()
                                                              : -std::numeric_limits<float>::max());
                }
            }
            // RNE/RMM 还有一段区间需要拦：中点 2^128-2^103 小于 Fmax=2^128-2^104，
            // 所以 exact 落在 [中点, Fmax] 时，上面那个"> Fmax"的分支进不来
            if ((rmode == 0 || rmode == 4) && std::isfinite(exact)
                && std::fabs(exact) >= 0x1p128 - 0x1p103)
                return exact > 0 ? std::numeric_limits<float>::infinity()
                                 : -std::numeric_limits<float>::infinity();
            // 两个数精确抵消时，IEEE 754 §6.3 只有 roundTowardNegative 给 -0，
            // 其余模式（含 RTZ）都是 +0。C 的加法固定产生 +0，这里补上符号
            // 不能靠 exact 的符号来判：它是宿主 double 运算的结果，
            // 宿主被 fesetround 改成 RDN 时抵消的结果已经是 -0了
            if (exact == 0.0 && cancelled_to_zero) {
                // 只有 roundTowardNegative 给 -0，其余模式必须强制 +0：
                // exact 本身可能已经是 -0（宿主舍入方向被改过）
                return rmode == 2 ? -0.0f : 0.0f;
            }
            float r = (float) exact;
            if (!std::isfinite(r)) return r;
            double back = (double) r;
            if (back == exact) return r;
            switch (rmode) {
            case 1: // RTZ
                return std::fabs(back) > std::fabs(exact) ? std::nextafterf(r, 0.0f) : r;
            case 2: // RDN
                return back > exact ? std::nextafterf(r, -std::numeric_limits<float>::infinity()) : r;
            case 3: // RUP
                return back < exact ? std::nextafterf(r, std::numeric_limits<float>::infinity()) : r;
            case 4: { // RMM：就近，中点向外
                float away = std::nextafterf(r, exact > 0 ? std::numeric_limits<float>::infinity()
                                                          : -std::numeric_limits<float>::infinity());
                // 中点时两边距离相等，RMM 要取向外的那个，所以必须用 <=
                return std::fabs((double) away - exact) <= std::fabs(back - exact) ? away : r;
            }
            default: return r; // RNE
            }
        };
        // rm 在浮点指令里是 funct3，这里取它的字段
        uint32_t f3_field = (inst >> RV::SH_F3) & RV::M_F3;
        int cur_rm = (f3_field == 7) ? (int) ((s.fcsr >> 5) & 7u) : (int) f3_field;
    switch (op) {
    case RV::OP_IMM: {
        // RV32 的移位量是 5 位、shamt[5] 属于 funct7；RV64 是 6 位、funct6 只占 inst[31:26]。
        // 用 if constexpr 静态分发：另一档的判据根本不会进本实例的代码
        uint32_t shamt = 0;
        bool is_srai = false;
        bool bad_enc = false;
        if constexpr (XLEN == 64) {
            // rs2 只取到 inst[24:20]，第 6 位在 inst[25]（funct6 把它移出去了）
            shamt = ((f7 & 1) << 5) | (rs2 & 0x1Fu);
            const uint32_t funct6 = f7 >> 1;
            is_srai = (funct6 == 0x10);
            if (f3 == RV::F3_SLLI && funct6 != 0) bad_enc = true;
            if (f3 == RV::F3_SRLI && funct6 != 0 && funct6 != 0x10) bad_enc = true;
        } else {
            shamt = rs2 & 0x1Fu;
            is_srai = (f7 == RV::F7_SUB_SRA);
            if (f3 == RV::F3_SLLI && f7 != 0) bad_enc = true;
            if (f3 == RV::F3_SRLI && f7 != 0 && (uint32_t) f7 != RV::F7_SUB_SRA) bad_enc = true;
        }
        // ---- Zbb / Zbs 的 OP-IMM 形式 ----
        // 这些指令的 funct7 与基础 OP-IMM 不同，必须在 bad_enc 之前分流，
        // 否则会被当成保留编码。RV64 的 shamt 占 6 位，第 6 位落在 f7 的最低位，
        // 所以用 (f7 & ~1) 比较、shamt 从 inst[25:20] 取
        {
            const uint32_t shamt6 = ((f7 & 1u) << 5) | (rs2 & 0x1Fu);
            const uint32_t f7b = f7 & ~1u;
            // OP-IMM 里对 addi/xori 这些非移位指令，inst[31:25] 是立即数高位、不是 funct7。
            // 只有 f3 落在 1/2/3/5（移位与位操作编码位）时才可能是 Z 扩展，
            // 否则必须完全跳过，否则 addi ra, ra, 700 这种会被误判
            // 只有 f3=1/5 才可能是 Z 扩展。f3=2/3 是 SLTI/SLTIU，此时 inst[31:25]
            // 是立即数高位而非 funct7，放进来会让某些立即数被误判成 Zbb/Zbs
            const bool z_imm_f3 = (f3 == 1 || f3 == 5);
            // Zbkb 的 zip/unzip 也在 OP-IMM：funct7=0x04、rs2=0，f3=1 是 zip、5 是 unzip。
            // 语义是把低半与高半的位交错/解交错
            // zip/unzip 的 rs2 不是 0 而是固定编码 0x0F（与 rev8 的 0x18 同类），
            // 所以只能按 (funct7, funct3) 精确匹配
            if (f7 == RV::F7_ZUW && (f3 == 1 || f3 == 5)) {
                const int half = XLEN / 2;
                XLen r = 0;
                for (int i = 0; i < half; ++i) {
                    if (f3 == 1) {          // zip：低半第 i 位 -> 第 2i 位，高半 -> 第 2i+1 位
                        r |= (XLen)((v1 >> i) & 1u) << (2 * i);
                        r |= (XLen)((v1 >> (half + i)) & 1u) << (2 * i + 1);
                    } else {                // unzip：反过来
                        r |= (XLen)((v1 >> (2 * i)) & 1u) << i;
                        r |= (XLen)((v1 >> (2 * i + 1)) & 1u) << (half + i);
                    }
                }
                s.regs[rd] = r;
                break;
            }
            if (z_imm_f3 && f7b == 0x30u) {
                // f3=1: clz/ctz/cpop/sext.b/sext.h；f3=5: rori
                if (f3 == 1) {
                    switch (rs2 & 0x1F) {
                    case 0: {   // clz
                        uint32_t n = 0; for (int b = XLEN - 1; b >= 0; --b) { if ((v1 >> b) & 1) break; ++n; }
                        s.regs[rd] = n; break;
                    }
                    case 1: {   // ctz
                        uint32_t n = 0; for (int b = 0; b < XLEN; ++b) { if ((v1 >> b) & 1) break; ++n; }
                        s.regs[rd] = n; break;
                    }
                    case 2: {   // cpop
                        uint32_t n = 0; for (int b = 0; b < XLEN; ++b) n += (uint32_t)((v1 >> b) & 1);
                        s.regs[rd] = n; break;
                    }
                    case 4: s.regs[rd] = (XLen)(SXLen)(int8_t)(v1 & 0xFF); break;    // sext.b
                    case 5: s.regs[rd] = (XLen)(SXLen)(int16_t)(v1 & 0xFFFF); break;  // sext.h
                    default: [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                    }
                    break;
                }
                if (f3 == 5) {   // rori
                    const uint32_t sh = shamt6 & (XLEN - 1);
                    s.regs[rd] = (XLen)(((uint64_t)v1 >> sh) | (XLen)(v1 << ((XLEN - sh) & (XLEN - 1))));
                    break;
                }
            }
            if (z_imm_f3 && f7b == 0x14u) {
                // f3=1/2/3/5: bseti/bclri/binvi/bexti；f3=5 且 rs2=7 是 orc.b
                if (f3 == 5 && (rs2 & 0x1F) == 7) {   // orc.b
                    XLen r = 0;
                    for (int b = 0; b < XLEN; b += 8)
                        if ((v1 >> b) & 0xFF) r |= (XLen)((XLen)0xFF << b);
                    s.regs[rd] = r; break;
                }
                // funct7=0x14 上只有 bseti（f3=1）；orc.b 已在上面单独处理
                if (f3 != 1) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                s.regs[rd] = v1 | ((XLen)1 << (shamt6 & (XLEN - 1)));   // bseti
                break;
            }
            // funct7=0x24 上：bclri(f3=1)、bexti(f3=5)
            if (z_imm_f3 && f7b == RV::F7_ZBS_BCLR) {
                const uint32_t sh3 = shamt6 & (XLEN - 1);
                if (f3 == 1) s.regs[rd] = v1 & ~((XLen)1 << sh3);        // bclri
                else if (f3 == 5) s.regs[rd] = (v1 >> sh3) & 1;          // bexti
                else [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                break;
            }
            // funct7=0x34 上：binvi(f3=1)、rev8 与 brev8(f3=5，靠 rs2 区分)
            if (z_imm_f3 && f7b == RV::F7_ZBS_BINV) {
                if (f3 == 1) { s.regs[rd] = v1 ^ ((XLen)1 << (shamt6 & (XLEN - 1))); break; }   // binvi
                if (f3 == 5) {
                    if ((rs2 & 0x1F) == RV::ZSUB_REV8) {          // rev8：整寄存器按字节反转
                        XLen r = 0;
                        for (int b = 0; b < XLEN; b += 8) {
                            int src = XLEN - 8 - b;
                            r |= (XLen)(((v1 >> src) & 0xFF) << b);
                        }
                        s.regs[rd] = r; break;
                    }
                    if ((rs2 & 0x1F) == RV::ZSUB_BREV8) {          // brev8：每字节内位反转
                        XLen r = 0;
                        for (int b = 0; b < XLEN; b += 8) {
                            uint32_t byte = (uint32_t)((v1 >> b) & 0xFF);
                            uint32_t rev = 0;
                            for (int i = 0; i < 8; ++i) rev |= ((byte >> i) & 1u) << (7 - i);
                            // 必须强转成 XLen 再左移：b 会到 56，uint32_t 左移超过 31 是 UB
                            r |= (XLen) rev << b;
                        }
                        s.regs[rd] = r; break;
                    }
                }
                [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
            }
        }
        if (bad_enc) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        // 立即数一律按有符号扩展到 XLEN：RV64 下负立即数不能零扩展
        const XLen imm = (XLen) ii;
        switch(f3){
        case RV::F3_ADDI:s.regs[rd]=v1+imm; break;
        case RV::F3_SLLI:s.regs[rd]=v1<<shamt; break;
        case RV::F3_SLTI:s.regs[rd]=(s1<(SXLen)imm)?1:0; break;
        case RV::F3_SLTIU:s.regs[rd]=(v1<(XLen)imm)?1:0; break;
        case RV::F3_XORI:s.regs[rd]=v1^imm; break;
        case RV::F3_SRLI:s.regs[rd]=is_srai?(XLen)(s1>>shamt):v1>>shamt; break;
        case RV::F3_ORI:s.regs[rd]=v1|imm; break;
        case RV::F3_ANDI:s.regs[rd]=v1&imm; break;
        } break;
    }
    case RV::LUI: s.regs[rd]=(XLen)(SXLen)iu; break;
    case RV::AUIPC: s.regs[rd]=s.pc+(XLen)(SXLen)iu; break;
    case RV::OP: {
    // ---- Zba / Zbb / Zbs：funct7 与基础运算不同，先在这里分流 ----
    // 这些扩展的编码复用 OP，靠 funct7 区分，必须在下面的 funct7 白名单之前处理
    // Zbc 的 clmul 系列也用 funct7=0x05，占 f3=1/2/3；max/min 占 4/5/6/7，
    // 所以这里的判断必须带 f3，否则会把 clmul 一起 trap 掉
    if (f7 == 0x05 && (f3 == 4 || f3 == 5 || f3 == 6 || f3 == 7)) {   // MAX/MAXU/MIN/MINU
        const bool sg = (f3 == 6 || f3 == 4);
        if (sg) {
            if (f3 == 6) s.regs[rd] = ((SXLen)v1 > (SXLen)v2) ? v1 : v2;   // MAX
            else         s.regs[rd] = ((SXLen)v1 < (SXLen)v2) ? v1 : v2;   // MIN
        } else {
            if (f3 == 7) s.regs[rd] = (v1 > v2) ? v1 : v2;                 // MAXU
            else if (f3 == 5) s.regs[rd] = (v1 < v2) ? v1 : v2;            // MINU
            else [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        }
        break;
    }
    if (f7 == 0x10) {          // Zba：sh1add / sh2add / sh3add
        switch (f3) {
        case 2: s.regs[rd] = (XLen)(v1 << 1) + v2; break;
        case 4: s.regs[rd] = (XLen)(v1 << 2) + v2; break;
        case 6: s.regs[rd] = (XLen)(v1 << 3) + v2; break;
        default: [[unlikely]] takeTrap(s, RV::EXC_II, inst); return;
        }
        break;
    }
    // Zbs 的寄存器形式分三组 funct7：bset=0x14、bclr/bext=0x24、binv=0x34。
    // 用 f3 再区分具体是哪条：单比特操作都是 f3=1，bext 是 f3=5
    if (f7 == RV::F7_ZBS_BSET || f7 == RV::F7_ZBS_BCLR || f7 == RV::F7_ZBS_BINV) {
        const uint32_t sh = (uint32_t)(v2 & (XLEN - 1));
        const XLen m = (XLen)1 << sh;
        if (f7 == RV::F7_ZBS_BSET && f3 == 1) s.regs[rd] = v1 | m;               // bset
        else if (f7 == RV::F7_ZBS_BCLR && f3 == 1) s.regs[rd] = v1 & ~m;         // bclr
        else if (f7 == RV::F7_ZBS_BINV && f3 == 1) s.regs[rd] = v1 ^ m;          // binv
        else if (f7 == RV::F7_ZBS_BCLR && f3 == 5) s.regs[rd] = (v1 >> sh) & 1;  // bext
        else [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        break;
    }
    // Zbb：andn/orn/xnor。funct7=0x20 同时是标准 SUB/SRA 的编码（f3=0/5），
    // 所以这里只处理 f3=4/6/7，其余必须落回下面的基础逻辑，不能 trap
    if (f7 == 0x20 && (f3 == 7 || f3 == 6 || f3 == 4)) {
        if (f3 == 7) s.regs[rd] = v1 & ~v2;        // andn
        else if (f3 == 6) s.regs[rd] = v1 | ~v2;   // orn
        else s.regs[rd] = ~(v1 ^ v2);              // xnor
        break;
    }
    if (f7 == 0x30) {          // Zbb：rol / ror
        const uint32_t sh = (uint32_t)(v2 & (XLEN - 1));
        if (f3 == 1) s.regs[rd] = (XLen)((v1 << sh) | (XLen)((uint64_t)v1 >> ((XLEN - sh) & (XLEN - 1))));
        else if (f3 == 5) s.regs[rd] = (XLen)(((uint64_t)v1 >> sh) | (XLen)(v1 << ((XLEN - sh) & (XLEN - 1))));
        else [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        break;
    }
    // Zbc 的 clmul 系列（funct7=0x05，与 max/min 共用，按 f3 区分：clmul=1/clmulh=3/clmulr=2）
    if (f7 == RV::F7_ZBC_CLMUL && (f3 == 1 || f3 == 2 || f3 == 3)) {
        unsigned __int128 acc = 0;
        for (int b = 0; b < XLEN; ++b)
            if ((v2 >> b) & 1) acc ^= (unsigned __int128)((uint64_t) v1) << b;
        if (f3 == 1) s.regs[rd] = (XLen) acc;                       // clmul
        else if (f3 == 3) s.regs[rd] = (XLen)(uint64_t)(acc >> XLEN); // clmulh
        // clmulr 取的是乘积的 bit[2N-2 : N-1]，不是简单的右移一位
        else s.regs[rd] = (XLen)(uint64_t)(acc >> (XLEN - 1));       // clmulr
        break;
    }
    // Zicond 的 czero（funct7=0x07）：eqz 是 f3=5、nez 是 f3=7
    if (f7 == RV::F7_ZICOND && (f3 == 5 || f3 == 7)) {
        if (f3 == 5) s.regs[rd] = (v2 == 0) ? 0 : v1;
        else         s.regs[rd] = (v2 != 0) ? 0 : v1;
        break;
    }
    // Zbkb 的 pack/packh 与 Zba 的 add.uw 共用 funct7=0x04，按 funct3 区分：
    // pack=4、packh=7、add.uw=0。这些编码是从测试 ELF 反汇编里核出来的
    if (f7 == RV::F7_ZUW) {
        if (f3 == 4) {   // pack：两侧各取低一半拼起来
            if constexpr (XLEN == 64)
                s.regs[rd] = (XLen)((((uint64_t) v1) & 0xFFFFFFFFULL) | ((((uint64_t) v2) & 0xFFFFFFFFULL) << 32));
            else
                s.regs[rd] = (XLen)((((uint64_t) v1) & 0xFFFFULL) | ((((uint64_t) v2) & 0xFFFFULL) << 16));
            break;
        }
        if (f3 == 7) {   // packh：两侧各取低 8 位
            s.regs[rd] = (XLen)((((uint64_t) v1) & 0xFFULL) | ((((uint64_t) v2) & 0xFFULL) << 8));
            break;
        }
        [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
    }
    // funct7 的合法值取决于 funct3，必须在 switch 之前统一判断：
    // f3=0 是 ADD/SUB（0x00/0x20）、f3=5 是 SRL/SRA（0x00/0x20）、
    // f3=1 是 SLL（只能 0x00）、其余 funct3 只能 0x00。
    // M 扩展占用的 0x01 单独放行，否则 MULH/DIVU 会被误判成保留编码。
    if (f7 != 0x01) {
        if (f7 != 0 && (uint32_t) f7 != RV::F7_SUB_SRA) [[unlikely]]
        { takeTrap(s, RV::EXC_II, inst); return; }
        if (f3 == RV::F3_SLL && f7 != 0) [[unlikely]]
        { takeTrap(s, RV::EXC_II, inst); return; }
        if (f3 != RV::F3_ADD && f3 != RV::F3_SRL && f7 != 0) [[unlikely]]
        { takeTrap(s, RV::EXC_II, inst); return; }
    }
    // M 扩展：funct7=0x01 时 funct3 选择乘除法，与普通运算共用同一 opcode
    if (f7 == 0x01) {
        // 高位乘与除法边界的特判都随 XLEN 变：RV64 的乘积需要 128 位，
        // 除零和 INT_MIN/-1 的常量也得跟着位宽走，否则 C 的有符号除零/溢出是 UB
        if constexpr (XLEN == 64) {
            using i128 = __int128;
            using u128 = unsigned __int128;
            switch (f3) {
            case 0: s.regs[rd] = v1 * v2; break;                                       // MUL
            case 1: s.regs[rd] = (XLen)((i128) s1 * (i128) s2 >> 64); break;           // MULH
            case 2: s.regs[rd] = (XLen)(((i128) s1 * (i128)(XLen) v2) >> 64); break;   // MULHSU
            case 3: s.regs[rd] = (XLen)(((u128) v1 * (u128) v2) >> 64); break;         // MULHU
            case 4: s.regs[rd] = (v2 == 0) ? (XLen) -1
                        : (s1 == (SXLen)((XLen) 1 << 63) && v2 == (XLen) -1) ? (XLen)((XLen) 1 << 63)
                        : (XLen)(i128)(s1 / s2); break;                                // DIV
            case 5: s.regs[rd] = (v2 == 0) ? (XLen) -1 : (XLen)(v1 / v2); break;       // DIVU
            case 6: s.regs[rd] = (v2 == 0) ? v1
                        : (s1 == (SXLen)((XLen) 1 << 63) && v2 == (XLen) -1) ? 0
                        : (XLen)(i128)(s1 % s2); break;                                // REM
            case 7: s.regs[rd] = (v2 == 0) ? v1 : (XLen)(v1 % v2); break;              // REMU
            }
        } else {
            switch (f3) {
            case 0: s.regs[rd] = (uint32_t)((uint64_t) v1 * v2); break;
            case 1: s.regs[rd] = (uint32_t)((uint64_t)((int64_t) s1 * (int64_t) s2) >> 32); break;
            case 2: s.regs[rd] = (uint32_t)((uint64_t)((int64_t) s1 * (int64_t)(uint64_t) v2) >> 32); break;
            case 3: s.regs[rd] = (uint32_t)(((uint64_t) v1 * (uint64_t) v2) >> 32); break;
            case 4: s.regs[rd] = (v2 == 0) ? 0xFFFFFFFFu
                        : (v1 == 0x80000000u && v2 == 0xFFFFFFFFu) ? 0x80000000u
                        : (uint32_t)(s1 / s2); break;
            case 5: s.regs[rd] = (v2 == 0) ? 0xFFFFFFFFu : (v1 / v2); break;
            case 6: s.regs[rd] = (v2 == 0) ? v1
                        : (v1 == 0x80000000u && v2 == 0xFFFFFFFFu) ? 0u
                        : (uint32_t)(s1 % s2); break;
            case 7: s.regs[rd] = (v2 == 0) ? v1 : (v1 % v2); break;
            }
        }
        break;
    }
    switch(f3){
        case RV::F3_ADD:s.regs[rd]=(f7==RV::F7_SUB_SRA)?v1-v2:v1+v2; break;
        case RV::F3_SLL:s.regs[rd]=v1<<(v2&(XLen)(XLEN-1)); break;
        case RV::F3_SLT:s.regs[rd]=(s1<s2)?1:0; break;
        case RV::F3_SLTU:s.regs[rd]=(v1<v2)?1:0; break;
        case RV::F3_XOR:s.regs[rd]=v1^v2; break;
        // RV64 的移位量是 6 位；SRA 结果不能再强转 uint32_t，否则高位符号位会被清掉
        case RV::F3_SRL:s.regs[rd]=(f7==RV::F7_SUB_SRA)?(XLen)(s1>>(v2&(XLen)(XLEN-1))):v1>>(v2&(XLen)(XLEN-1)); break;
        case RV::F3_OR:s.regs[rd]=v1|v2; break;
        case RV::F3_AND:s.regs[rd]=v1&v2; break;
        } break;
    }
    // RV64 的 W 系列立即数指令（opcode 0x1B）：立即数占 inst[31:20]，
    // 移位类用 inst[25:20] 当 5 位移位量。这里不能套用 R 型的 funct7 检查——
    // 立即数高位会被误当成 funct7，addiw x, x, -1 这种就会被判成非法指令
    case RV::OP_IMM_32: {
        if constexpr (XLEN != 64) { takeTrap(s, RV::EXC_II, inst); return; }
        else {
            const uint32_t lo1 = (uint32_t) v1;
            const uint32_t sh = rs2 & 0x1F;
            // ---- Zba/Zbb 的 W 形式：funct7 与基础 OP-IMM-32 不同，先分流 ----
            // slli.uw 的编码是 funct7=0x04（与 add.uw 同组），不是 0x02——这是从 ELF 反汇编核出来的
            // slli.uw 的移位量是 6 位，shamt[5] 落在 funct7 的 bit0 里，
            // 所以要用 (f7 & ~1) 匹配、移位量由 inst[25:20] 拼出来
            if ((f7 & ~1u) == RV::F7_ZUW) {
                if (f3 != 1) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                s.regs[rd] = (XLen) lo1 << (((f7 & 1u) << 5) | (rs2 & 0x1Fu));
                break;
            }
            if ((f7 & ~1u) == 0x30u) {
                if (f3 == 1) {
                    switch (rs2 & 0x1F) {
                    case 0: { uint32_t n = 0; for (int b = 31; b >= 0; --b) { if ((lo1 >> b) & 1) break; ++n; } s.regs[rd] = n; break; }   // clzw
                    case 1: { uint32_t n = 0; for (int b = 0; b < 32; ++b) { if ((lo1 >> b) & 1) break; ++n; } s.regs[rd] = n; break; }    // ctzw
                    case 2: { uint32_t n = 0; for (int b = 0; b < 32; ++b) n += (lo1 >> b) & 1; s.regs[rd] = n; break; }                    // cpopw
                    default: [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                    }
                    break;
                }
                if (f3 == 5) {   // roriw
                    const uint32_t sh2 = rs2 & 0x1F;
                    const uint32_t r = (lo1 >> sh2) | (lo1 << ((32 - sh2) & 31));
                    s.regs[rd] = (XLen)(SXLen)(int32_t) r;
                    break;
                }
            }
            if ((f3 == RV::F3_SLLI || f3 == RV::F3_SRLI) && f7 != 0 && f7 != 0x20) [[unlikely]]
            { takeTrap(s, RV::EXC_II, inst); return; }
            switch (f3) {
            case RV::F3_ADDI: s.regs[rd] = (XLen)(SXLen)(int32_t)(lo1 + (uint32_t) ii); break; // ADDIW
            case RV::F3_SLLI: s.regs[rd] = (XLen)(SXLen)(int32_t)(lo1 << sh); break;          // SLLIW
            case RV::F3_SRLI: s.regs[rd] = (f7 == 0x20)
                        ? (XLen)(SXLen)(int32_t)((int32_t) lo1 >> sh)                          // SRAIW
                        : (XLen)(SXLen)(int32_t)(lo1 >> sh); break;                            // SRLIW
            default: takeTrap(s, RV::EXC_II, inst); return;
            }
            break;
        }
    }
    // RV64 的 W 系列 R 型指令（opcode 0x3B）：结果按 32 位算完再符号扩展到 XLEN
    case RV::OP_32: {
        if constexpr (XLEN != 64) { takeTrap(s, RV::EXC_II, inst); return; }
        else {
            const bool sub = (f7 == RV::F7_SUB_SRA);
            const uint32_t lo1 = (uint32_t) v1, lo2 = (uint32_t) v2;
            // ---- Zba/Zbb 的 W 形式 ----
            // 0x3B 上 funct7=0x04 的一组：add.uw(f3=0)、zext.h 与 packw(f3=4，靠 rs2 区分)
            if (f7 == RV::F7_ZUW) {
                if (f3 == 4) {
                    if (lo2 == 0) { s.regs[rd] = (XLen)(v1 & 0xFFFF); break; }   // zext.h（rs2=0）
                    uint32_t r = (lo1 & 0xFFFF) | ((lo2 & 0xFFFF) << 16);         // packw
                    s.regs[rd] = (XLen)(SXLen)(int32_t) r;
                    break;
                }
                if (f3 != 0) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                s.regs[rd] = (XLen)(uint32_t) v1 + v2;   // add.uw
                break;
            }
            // Zba 的 .uw 形式：sh1add.uw/sh2add.uw/sh3add.uw 与 sh1add 同 funct7，靠 opcode 区分
            if (f7 == RV::F7_ZBA_SHADD) {
                const XLen basew = (XLen)(uint32_t) v1;
                if (f3 == 2) s.regs[rd] = (XLen)(basew << 1) + v2;
                else if (f3 == 4) s.regs[rd] = (XLen)(basew << 2) + v2;
                else if (f3 == 6) s.regs[rd] = (XLen)(basew << 3) + v2;
                else [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                break;
            }
            if ((f7 & ~1u) == 0x10u) {   // sh1add.uw / sh2add.uw / sh3add.uw
                const XLen base = (XLen)(uint32_t) v1;
                if (f3 == 2) s.regs[rd] = (XLen)(base << 1) + v2;
                else if (f3 == 4) s.regs[rd] = (XLen)(base << 2) + v2;
                else if (f3 == 6) s.regs[rd] = (XLen)(base << 3) + v2;
                else [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                break;
            }
            if (f7 == 0x30) {   // rolw / rorw
                const uint32_t sh2 = (uint32_t)(v2 & 31);
                uint32_t r;
                if (f3 == 1) r = (lo1 << sh2) | (lo1 >> ((32 - sh2) & 31));
                else if (f3 == 5) r = (lo1 >> sh2) | (lo1 << ((32 - sh2) & 31));
                else [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
                s.regs[rd] = (XLen)(SXLen)(int32_t) r;
                break;
            }
            if (f7 == 0x01) {
                const int32_t sl1 = (int32_t) lo1, sl2 = (int32_t) lo2;
                switch (f3) {
                case 0: s.regs[rd] = (XLen)(SXLen)(int32_t)(lo1 * lo2); break;                 // MULW
                case 4: s.regs[rd] = (lo2 == 0) ? (XLen) -1
                            : (sl1 == INT32_MIN && sl2 == -1) ? (XLen)(SXLen)INT32_MIN
                            : (XLen)(SXLen)(int32_t)(sl1 / sl2); break;                        // DIVW
                case 5: s.regs[rd] = (lo2 == 0) ? (XLen)(SXLen)(int32_t) -1
                            : (XLen)(SXLen)(int32_t)(lo1 / lo2); break;                        // DIVUW
                case 6: s.regs[rd] = (lo2 == 0) ? (XLen)(SXLen)sl1
                            : (sl1 == INT32_MIN && sl2 == -1) ? 0
                            : (XLen)(SXLen)(int32_t)(sl1 % sl2); break;                        // REMW
                case 7: s.regs[rd] = (lo2 == 0) ? (XLen)(SXLen)sl1
                            : (XLen)(SXLen)(int32_t)(lo1 % lo2); break;                        // REMUW
                default: takeTrap(s, RV::EXC_II, inst); return;
                }
                break;
            }
            if (f7 != 0 && !sub) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
            switch (f3) {
            case RV::F3_ADD: s.regs[rd] = (XLen)(SXLen)(int32_t)(sub ? lo1 - lo2 : lo1 + lo2); break; // ADDW/SUBW
            case RV::F3_SLL: s.regs[rd] = (XLen)(SXLen)(int32_t)(lo1 << (lo2 & 0x1F)); break;        // SLLW
            case RV::F3_SRL: s.regs[rd] = sub
                        ? (XLen)(SXLen)(int32_t)((int32_t) lo1 >> (lo2 & 0x1F))                      // SRAW
                        : (XLen)(SXLen)(int32_t)(lo1 >> (lo2 & 0x1F)); break;                        // SRLW
            default: takeTrap(s, RV::EXC_II, inst); return;
            }
            break;
        }
    }
    case RV::JAL: {
        XLen tgt=s.pc+(XLen)(SXLen)ij;
        if (tgt & 1u) [[unlikely]] { takeTrap(s, RV::EXC_IAM, tgt); return; } // 有 C 扩展时 IALIGN=16，只禁止奇地址
        s.regs[rd]=s.pc+s.inst_len; npc=tgt; break;
    }
    case RV::JALR: {
        if(f3!=0) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        // JALR 的目标 bit0 由硬件清零，规范明确规定不会因为它而 trap——
        // 软件靠这个特性省掉一条清位指令，所以这里不能检查 bit0
        const XLen tgt = (v1 + (XLen)(SXLen)ii) & ~(XLen)1;
        s.regs[rd]=s.pc+s.inst_len; npc=tgt; break;
    }
    case RV::BRANCH: { if (f3 == 2 || f3 == 3) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        bool t=false; switch(f3){
        case RV::F3_BEQ:t=(v1==v2);break; case RV::F3_BNE:t=(v1!=v2);break;
        case RV::F3_BLT:t=(s1<s2);break; case RV::F3_BGE:t=(s1>=s2);break;
        case RV::F3_BLTU:t=(v1<v2);break; case RV::F3_BGEU:t=(v1>=v2);break;
        } if(t) { XLen tgt=s.pc+(XLen)(SXLen)ib;
            if (tgt & 1u) [[unlikely]] { takeTrap(s, RV::EXC_IAM, tgt); return; }   // IALIGN=16
            npc=tgt; } } break;

    // ---- LOAD ----
    case RV::LOAD: {
        // RV32 的 load 只定义了 0/1/2/4/5；RV64 多出 f3=3(LD) 和 6(LWU)
        const bool load_ok = (XLEN == 64) ? (f3 != 7) : (f3 != 3 && f3 <= 5);
        if (!load_ok) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }

        XLen va=v1+(XLen)(SXLen)ii, pa;

        if (!mmuTranslate(s, va, pa, false, false)) [[unlikely]] return; // 缺页已 trap
        // 数据断点：命中则这条访存不执行，直接进 breakpoint 陷阱
        if (triggerMatch(s, va, RV::TRIG_LOAD)) [[unlikely]] { takeTrap(s, RV::EXC_BP, va); return; }
        // 总线只接受字对齐地址，字内偏移由 CPU 自己记着；跨字则在 WAIT_MEM 里再读一个字
        uint32_t p=pa&3;
        uint32_t n=(f3==RV::F3_LH||f3==RV::F3_LHU)?2:(f3==RV::F3_LB||f3==RV::F3_LBU)?1:(f3==3)?8:4;
        // MPRV 下 M 态的数据访问同样走分页，此时跨页检查不能因为 priv==M 就跳过，
        // 否则非对齐拆分会对第二个字直接用物理地址，跳过了另外一页的翻译
        bool _paging_active = pagingEnabled<XLEN>(s.satp) &&
                              (s.priv != RV::PRIV_M ||
                               ((s.mstatus & RV::MSTATUS_MPRV) &&
                                (((s.mstatus >> 11) & 3u) != RV::PRIV_M)));
// 跨字访问的第二次事务地址会越过地址空间顶端时回绕到 0，从设备读到的是
        // 不相干的数据，且不会触发任何 trap。总线只有 32 位地址线，两种位宽都要拦
        if (p + n > 4) {
        const XLen a2 = (pa & ~(XLen)3) + 4;
        if (a2 < pa || a2 > (XLen)0xFFFFFFFF) { takeTrap(s, RV::EXC_LAM, va); return; }
        }
        
        if (_paging_active && ((va & 0xFFF) + n > 0x1000)) {
            // 跨页时第二个字不属于同一个物理页，需要第二次翻译（当前状态机不支持），
            // 按规范允许的做法触发 misaligned load。必须在置 need_mem 之前返回，
            // 否则 EXECUTE 之后仍会按残留的 need_mem 发起一次错误访存。
            s.acc_stage = 0;
            takeTrap(s, RV::EXC_LAM, va);
            return;
        }
        wb_adr=pa&~(XLen)3; wb_we=false; wb_sel=0x0F;
        wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa&~(XLen)3; s.bus_funct3=(uint8_t)f3; s.bus_rd=(uint8_t)rd; s.bus_sel=0x0F; s.bus_fp=0;
        s.acc_va=pa; s.acc_f3=(uint8_t)f3; s.acc_stage=(uint8_t)((p+n>4)?1:0);
        } break; // 不 return，让 PC 更新 + need_mem 走 WAIT_MEM

    // ---- STORE ----
    case RV::STORE: {
        // RV64 多出 f3=3(SD)
        if (f3 > 2 && !(XLEN == 64 && f3 == 3)) [[unlikely]]
        { takeTrap(s, RV::EXC_II, inst); return; }

        XLen va=v1+(XLen)(SXLen)is, pa;

        if (!mmuTranslate(s, va, pa, true, false)) [[unlikely]] return;
        if (triggerMatch(s, va, RV::TRIG_STORE)) [[unlikely]] { takeTrap(s, RV::EXC_BP, va); return; }
        // 总线只接受字对齐地址：先写字内剩余部分，跨字的高段留到 WAIT_MEM 补写
        uint32_t p=pa&3;
        uint32_t n=(f3==RV::F3_SB)?1:(f3==RV::F3_SH)?2:(f3==3)?8:4;
        uint32_t first=(p+n>4)?(4-p):n;
        // MPRV 下 M 态的数据访问同样走分页，此时跨页检查不能因为 priv==M 就跳过，
        // 否则非对齐拆分会对第二个字直接用物理地址，跳过了另外一页的翻译
        bool _paging_active = pagingEnabled<XLEN>(s.satp) &&
                              (s.priv != RV::PRIV_M ||
                               ((s.mstatus & RV::MSTATUS_MPRV) &&
                                (((s.mstatus >> 11) & 3u) != RV::PRIV_M)));
// 跨字访问的第二次事务地址会越过地址空间顶端时回绕到 0，从设备读到的是
        // 不相干的数据，且不会触发任何 trap。总线只有 32 位地址线，两种位宽都要拦
        if (p + n > 4) {
        const XLen a2 = (pa & ~(XLen)3) + 4;
        if (a2 < pa || a2 > (XLen)0xFFFFFFFF) { takeTrap(s, RV::EXC_SAM, va); return; }
        }
        
        if (_paging_active && ((va & 0xFFF) + n > 0x1000)) {
            s.acc_stage = 0;
            takeTrap(s, RV::EXC_SAM, va); // 跨页非对齐写同上，只能报错
            return;
        }
        wb_adr=pa&~(XLen)3; wb_we=true;
        wb_sel=(uint8_t)(((1u<<first)-1u)<<p); wb_dat=v2<<(8*p);
        s.acc_va=pa; s.acc_raw=v2; s.acc_f3=(uint8_t)f3; s.acc_stage=(uint8_t)((n>first)?1:0);
        if (s.lr_valid) { // 同 hart 存储覆盖保留地址所在的字即失效
            XLen w0 = pa & ~(XLen)3;
            if (s.lr_addr == w0 || (s.acc_stage == 1 && s.lr_addr == w0 + 4)) s.lr_valid = false;
        }
        wb_stb=wb_cyc=true; need_mem=true; s.bus_adr=pa&~(XLen)3; s.bus_wdat=wb_dat; s.bus_we=true; s.bus_sel=wb_sel; s.bus_funct3=0; s.bus_rd=0;
        } break; // 不 return，让 PC 更新 + need_mem 走 WAIT_MEM

    // ---- AMO ----
    case RV::AMO: {
        s.acc_stage = 0; // AMO 是单次对齐事务，不能继承上一次非对齐访存的拆分级数
        s.bus_fp = 0;    // 清掉可能残留的浮点访存标志，否则读响应会写进 fregs
        // .W 是 funct3=2，.D 是 funct3=3（只有 RV64 有）
        // f3=2 是 FLW（4 字节），f3=3 是 FLD（8 字节）。D 扩展与 XLEN 无关：
        // RV32 的 f 寄存器同样是 64 位，fld/fsd 一样可用
        if (f3 != 2 && f3 != 3) [[unlikely]]
        { takeTrap(s, RV::EXC_II, inst); return; }
        switch ((inst >> 27) & 0x1F) { // 未实现的 AMO 编码必须报非法指令，不能静默完成
        case RV::F5_LR: case RV::F5_SC: case RV::F5_AMOSWAP: case RV::F5_AMOADD:
        case RV::F5_AMOXOR: case RV::F5_AMOAND: case RV::F5_AMOOR:
        case RV::F5_AMOMIN: case RV::F5_AMOMAX: case RV::F5_AMOMINU: case RV::F5_AMOMAXU:
            break;
        default: [[unlikely]] takeTrap(s, RV::EXC_II, inst); return;
        }
        // AMO/LR/SC 属于 store/AMO 类，misaligned 的 cause 是 6 不是 4
        // LR 是 load 类（cause 4），SC 和其余 AMO 才是 store/AMO 类（cause 6）
        bool amo_is_load = (((inst >> 27) & 0x1F) == RV::F5_LR);
        const bool is_d = (f3 == 3);
        // .D 要求 8 字节对齐
        const XLen amo_align = is_d ? (XLen) 7 : (XLen) 3;
        if (v1 & amo_align) [[unlikely]]
        { takeTrap(s, amo_is_load ? RV::EXC_LAM : RV::EXC_SAM, v1); return; }
        XLen va=v1, pa;
        // LR 只读，只要求页有 R；SC 与其余 AMO 要写，要求 W
        bool amo_write = (((inst >> 27) & 0x1F) != RV::F5_LR);
        if (!mmuTranslate(s, va, pa, amo_write, false)) [[unlikely]] return;
        // LR 属于 load 类，SC/其余 AMO 属于 store 类
        if (triggerMatch(s, va, amo_is_load ? RV::TRIG_LOAD : RV::TRIG_STORE)) [[unlikely]]
        { takeTrap(s, RV::EXC_BP, va); return; }
        // AMO: 先发起读请求，WAIT_MEM 中完成修改后自动回写
        wb_adr=pa&~(XLen)3; wb_dat=v2; wb_we=false; wb_sel=0x0F;
        wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa; s.bus_funct3=f3; s.bus_rd=rd;
        s.bus_sel=0x0F; s.bus_we=false; s.bus_amo=true;
        s.bus_amo_f5=(uint8_t)((inst>>27)&0x1F); // funct5
        s.bus_wdat=v2;      // 低 32 位，.W 用它
        s.amo_rs2 = v2;     // bus_wdat 装不下 64 位，.D 的 rs2 从这里取
        s.amo_is_d = is_d;  // .D 要分两段读、两段写
        s.amo_hi = 0;
        s.amo_hi_done = false;
        s.amo_hi_write = false;
        } break; // 不 return，让 PC 更新 + need_mem 走 WAIT_MEM

    // ---- 浮点访存（FLH/FLW/FLD、FSH/FSW/FSD，自然对齐；非对齐按规范触发异常）----
    case RV::LOAD_FP: {
        // FLW/FLD 写完 freg 后 FS 必须是 Dirty，否则上下文切换不会保存浮点寄存器
        if (((s.mstatus >> 13) & 3u) == 0) { takeTrap(s, RV::EXC_II, inst); return; } // FS=Off 时禁止浮点指令
        // f3=1 是 FLH（2 字节，Zfh），f3=2 是 FLW（4 字节），f3=3 是 FLD（8 字节）
        if (f3 != 1 && f3 != 2 && f3 != 3) [[unlikely]]
        { takeTrap(s, RV::EXC_II, inst); return; }
        XLen va=v1+(XLen)(SXLen)ii, pa;
        if (!mmuTranslate(s, va, pa, false, false)) [[unlikely]] return;
        if (triggerMatch(s, va, RV::TRIG_LOAD)) [[unlikely]] { takeTrap(s, RV::EXC_BP, va); return; }
        // FLH 只要求 2 字节对齐，其余要求 4 字节
        if (pa & ((f3 == 1) ? (XLen)1 : (XLen)3)) [[unlikely]] { takeTrap(s, RV::EXC_LAM, va); return; }
        // 总线只接受字对齐地址：FLH 允许 2 字节对齐（可能落在 4n+2），
        // 地址要抹掉低两位，具体取哪半由 acc_va 在读回时移位决定
        wb_adr=pa & ~(XLen)3; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa; s.bus_funct3=(uint8_t)f3; s.bus_rd=(uint8_t)rd; s.bus_fp=1;
        // 读响应按 acc_va 的低两位移位，不记就会用上一条访存的残留偏移
        s.acc_va=pa; s.acc_stage = 0;
        // FLD 要 8 字节：总线只有 32 位，第二段复用非对齐访存的拆段机制
        s.acc_f3 = (uint8_t)f3;
        if (f3 == 3) s.acc_stage = 1;
        s.mstatus |= (3u << 13); // 写 freg，FS 置 Dirty
        } break;

    case RV::STORE_FP: {
        if (((s.mstatus >> 13) & 3u) == 0) { takeTrap(s, RV::EXC_II, inst); return; } // FS=Off 时禁止浮点指令
        // f3=1 是 FSH（2 字节），f3=2 是 FSW，f3=3 是 FSD
        if (f3 != 1 && f3 != 2 && f3 != 3) [[unlikely]]
        { takeTrap(s, RV::EXC_II, inst); return; }
        XLen va=v1+(XLen)(SXLen)is, pa;
        if (!mmuTranslate(s, va, pa, true, false)) [[unlikely]] return;
        if (triggerMatch(s, va, RV::TRIG_STORE)) [[unlikely]] { takeTrap(s, RV::EXC_BP, va); return; }
        if (pa & ((f3 == 1) ? (XLen)1 : (XLen)3)) [[unlikely]] { takeTrap(s, RV::EXC_SAM, va); return; }
        if (s.lr_valid && s.lr_addr == (pa & ~(XLen)3)) s.lr_valid = false;
        // 总线只接受字对齐地址：FSH/FSW 按字内偏移搬字节，FSD 走拆段机制
        uint32_t p = (uint32_t)(pa & 3);
        uint32_t wdat = (f3 == 1) ? ((uint32_t)(uint16_t) s.fregs[rs2] << (8 * p))
                                  : ((uint32_t) s.fregs[rs2] << (8 * p));
        uint8_t sel = (f3 == 1) ? (uint8_t)(0x3u << p) : (uint8_t)(0xFu << p);
        wb_adr=pa & ~(XLen)3; wb_we=true; wb_sel=sel; wb_dat=wdat; wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa & ~(XLen)3; s.bus_wdat=wdat; s.bus_we=true; s.bus_sel=sel; s.bus_fp=0; s.acc_stage=0;
        // FSD 要写 8 字节，复用拆段机制：acc_raw 存待写数据，acc_f3=3 表示 8 字节
        if (f3 == 3) { s.acc_va = pa; s.acc_raw = s.fregs[rs2]; s.acc_f3 = 3; s.acc_stage = 1; }
        } break;

    // ---- 融合乘加：FMADD / FMSUB / FNMSUB / FNMADD ----
    case 0x43: case 0x47: case 0x4B: case 0x4F: {
        if (((s.mstatus >> 13) & 3u) == 0) { takeTrap(s, RV::EXC_II, inst); return; } // FS=Off 时禁止浮点指令
        // FMA 的 rm 在 funct3：5/6 是保留编码，7 表示取 fcsr.frm
        if ((f3 == 7 ? ((s.fcsr >> 5) & 7u) : f3) > 4) { takeTrap(s, RV::EXC_II, inst); return; }
        // 把操作数当浮点数用时才检查 NaN-box：高 32 位不是全 1 说明这不是合法的
        // 单精度值，按规范整体当作 canonical NaN。FMV.X.W / FCLASS.S 直接读位模式，
        // 走的是 s.fregs[rs1] 不经过这里，所以不受影响
        auto u2f = [](uint64_t v) -> float {
            uint32_t u = (v >> 32) == 0xFFFFFFFFu ? (uint32_t) v : 0x7FC00000u;
            float f; std::memcpy(&f, &u, 4); return f;
        };
        auto f2u = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        const uint32_t fma_fmt = (inst >> 25) & 3u;
        if (fma_fmt == 2u) {
            // 半精度 FMA：double 的 53 位尾数足以容纳 half 的 22 位乘积加上对齐余量，
            // std::fma 保证乘加只舍入一次，最后再按 rm 舍入到 half
            const uint32_t rs3h = (inst >> 27) & 0x1F;
            const uint32_t rmv = (f3 == 7) ? ((s.fcsr >> 5) & 7u) : f3;
            auto readH = [&](int idx) -> double {
                if ((s.fregs[idx] >> 16) != 0xFFFFFFFFFFFFULL)
                    return std::numeric_limits<double>::quiet_NaN();
                return halfToDouble((uint16_t) s.fregs[idx]);
            };
            double a = readH(rs1), b = readH(rs2), c = readH(rs3h);
            double m = (op == 0x43 || op == 0x47) ? std::fma(a, b, (op == 0x43) ? c : -c)
                                                  : std::fma(-a, b, (op == 0x4B) ? c : -c);
            {
                double p = a * b;
                double pe = std::fma(a, b, -p);
                double sp = p + c;
                double bb = sp - p;
                double se = (p - (sp - bb)) + (c - bb);
                if (std::isfinite(m) && (pe + se) != 0.0) s.fcsr |= 0x01;   // NX
            }
            if (std::isnan(m)) { s.fregs[rd] = 0xFFFFFFFFFFFF7E00ULL; s.fcsr |= 0x10; }
            else s.fregs[rd] = 0xFFFFFFFFFFFF0000ULL | (uint64_t) doubleToHalf(m, rmv, s.fcsr);
            s.mstatus |= (3u << 13); // 写 freg，FS 置 Dirty
            break;
        }
        if (fma_fmt == 1u) {
            // 双精度 FMA：宿主 double 就是 64 位 IEEE754，用 std::fma 保证"乘加只舍入一次"，
            // 这才是 FMA 相对先乘后加的意义。D 扩展与 XLEN 无关，RV32 同样支持
            {
                uint32_t rs3d = (inst >> 27) & 0x1F;
                auto u2d = [](uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; };
                auto d2u = [](double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; };
                double a = u2d(s.fregs[rs1]), b = u2d(s.fregs[rs2]), c = u2d(s.fregs[rs3d]);
                // 按 op 决定是 a*b±c 还是 -(a*b)±c
                double m = (op == 0x43 || op == 0x47) ? std::fma(a, b, (op == 0x43) ? c : -c)
                                                      : std::fma(-a, b, (op == 0x4B) ? c : -c);
                // fma 的结果本身是舍入过的，拿不到"是否精确"。用两条余项拼出精确误差：
                // pe 是 a*b 的余项，se 是 p+c 的 TwoSum 余项
                {
                    double p = a * b;
                    double pe = std::fma(a, b, -p);
                    double sp = p + c;
                    double bb = sp - p;
                    double se = (p - (sp - bb)) + (c - bb);
                    if (std::isfinite(m) && (pe + se) != 0.0) s.fcsr |= 0x01;   // NX
                }
                if (std::isnan(m)) { s.fregs[rd] = 0x7FF8000000000000ULL; s.fcsr |= 0x10; }
                else s.fregs[rd] = d2u(m);
                // 写了 freg 就必须把 FS 置 Dirty，否则上下文切换不会保存浮点寄存器；
                // 这里的 break 跳过了单精度路径末尾的同一句，不能漏
                s.mstatus |= (3u << 13);
                break;
            }
        }
        if (fma_fmt != 0u) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        uint32_t rs3 = (inst >> 27) & 0x1F;
        float a = u2f(s.fregs[rs1]), b = u2f(s.fregs[rs2]), c = u2f(s.fregs[rs3]);
        // double 中转不会引入双重舍入偏差：中间精度 p2 >= 2*p1+2 时两次舍入等价于一次，
        // 这里 53 >= 2*24+2 = 50 满足，且 float 乘 float 的 48 位尾数在 double 里是精确的
        double prod = (double)a * (double)b;
        double exact = (op == 0x43) ? prod + (double)c
                     : (op == 0x47) ? prod - (double)c
                     : (op == 0x4B) ? -prod + (double)c
                                    : -prod - (double)c;
        // prod 与 c 精确抵消时零的符号也要按模式定（只有相加形式会抵消）
        // FMADD=prod+c、FMSUB=prod-c、FNMSUB=-prod+c、FNMADD=-prod-c，
        // 抵消条件取决于实际表达式的符号
        // 不能用 prod != 0 排除零：(+0)+(-0) 这类也属于精确抵消，
        // 只有同号零相加才不是抵消
        // 零交叉时"异号"的含义取决于实际表达式：
        // prod+c / -prod-c 要异号才抵消，prod-c / -prod+c 要同号才抵消
        bool same_sign = (std::signbit(prod) == std::signbit(c));
        // 用 TwoProduct/TwoSum 的误差项判断 double 这一步是否丢了低位：
        // 指数差很大时（如 1.0 + 2^-100）(dobule)r != exact 检测不出来
        double prod_hi = (double) a * (double) b;
        double prod_err = std::fma((double) a, (double) b, -prod_hi); // 乘积的舍入误差
        double sum_hi = prod_hi + (double) c;
        double bv = sum_hi - prod_hi;
        double sum_err = (prod_hi - (sum_hi - bv)) + ((double) c - bv);
        bool fma_inexact = (prod_err != 0.0) || (sum_err != 0.0);
        bool fma_cancel = ((op == 0x43 || op == 0x4F) ? c == -prod : c == prod)
                          && (prod != 0.0 || c != 0.0
                              || (op == 0x43 || op == 0x4F ? !same_sign : same_sign));
        float r = round_to_float(exact, (f3 == 7) ? (int) ((s.fcsr >> 5) & 7u) : (int) f3, fma_cancel);
        s.fregs[rd] = std::isnan(r) ? 0x7FC00000u : f2u(r); // 同样规范化 NaN
        // sNaN 参与任何浮点运算都要报 NV，不只是"输入全不是 NaN"的非法运算
        bool fma_snan = ((s.fregs[rs1] >> 23) & 0xFF) == 0xFF && (s.fregs[rs1] & 0x7FFFFFu) && !(s.fregs[rs1] & 0x400000u)
                        || ((s.fregs[rs2] >> 23) & 0xFF) == 0xFF && (s.fregs[rs2] & 0x7FFFFFu) && !(s.fregs[rs2] & 0x400000u)
                        || ((s.fregs[rs3] >> 23) & 0xFF) == 0xFF && (s.fregs[rs3] & 0x7FFFFFu) && !(s.fregs[rs3] & 0x400000u);
        if (std::isnan(r)) {
            // 0*inf 无论加数是什么（含 qNaN）都是无效运算
            bool mul_invalid = (a == 0.0f && std::isinf(b)) || (std::isinf(a) && b == 0.0f);
            if (fma_snan || mul_invalid || (!std::isnan(a) && !std::isnan(b) && !std::isnan(c)))
                s.fcsr |= 0x10;
        } else if (std::isfinite(exact)) {
            if ((double)r != exact || fma_inexact) s.fcsr |= 0x01; // NX
            // OF：有限结果舍入成 inf；UF：非零结果落入非规格化数（0 < |r| < FLT_MIN），
            // 或直接舍入成 0 而精确结果非 0
            // UF 的定义是"tiny 且不精确"：精确的次正规数结果不能置 UF
            // RTZ/RDN(正)/RUP(负) 在溢出时返回的是 Fmax，isinf 盖不到，
            // 但按 IEEE 754 只要无界指数下的结果超出 Fmax 就要报 OF
            if (std::isinf(r) || std::fabs(exact) >= 0x1p128) s.fcsr |= 0x04;
            else if ((r == 0.0f ? exact != 0.0
                                : std::fabs((double) r) < (double) std::numeric_limits<float>::min())
                     && (double) r != exact)
                s.fcsr |= 0x02;
        }
        s.mstatus |= (3u << 13);
        break;
    }

    // ---- 单精度浮点运算（F 扩展），寄存器内容是 IEEE754 位模式 ----
    case RV::OP_FP: {
        // OP-FP 的 funct7[1:0] 是格式域：0=S 1=D 2=H（Zfh）。fmt=3（Q）没实现。
        const uint32_t fmt = f7 & 3u;
        // 按格式读写浮点寄存器。half 必须 NaN-boxing：读时 bits[63:16] 不是全 1
        // 说明这不是合法的半精度值，按 canonical NaN 处理；写时高 48 位填 1
        auto fregRead = [&](int idx, uint32_t f) -> double {
            if (f == 0) { uint32_t u = (uint32_t) s.fregs[idx]; float v; std::memcpy(&v, &u, 4); return v; }
            if (f == 1) { double v; std::memcpy(&v, &s.fregs[idx], 8); return v; }
            if ((s.fregs[idx] >> 16) != 0xFFFFFFFFFFFFULL)
                return std::numeric_limits<double>::quiet_NaN();
            return halfToDouble((uint16_t) s.fregs[idx]);
        };
        const uint32_t xrm = (f3 == 7) ? ((s.fcsr >> 5) & 7u) : f3;
        auto fregWrite = [&](int idx, uint32_t f, double v) {
            if (f == 0) {
                float fv = (float) v;
                uint32_t u; std::memcpy(&u, &fv, 4);
                s.fregs[idx] = 0xFFFFFFFF00000000ULL | (uint64_t) u;
            } else if (f == 1) {
                std::memcpy(&s.fregs[idx], &v, 8);
            } else {
                s.fregs[idx] = 0xFFFFFFFFFFFF0000ULL | (uint64_t) doubleToHalf(v, xrm, s.fcsr);
            }
        };
        // 跨精度转换（funct7 = 0x20|dst，rs2 = 源格式）
        if ((f7 & ~3u) == 0x20u) {
            const uint32_t dst = fmt, src = rs2;
            // 同格式或超出 S/D/H 都是保留编码
            if (dst > 2 || src > 2 || dst == src) { takeTrap(s, RV::EXC_II, inst); return; }
            if (((s.mstatus >> 13) & 3u) == 0) { takeTrap(s, RV::EXC_II, inst); return; }
            if (xrm > 4) { takeTrap(s, RV::EXC_II, inst); return; }
            if (dst == 0 && src == 1) {
                // FCVT.S.D：双精度 -> 单精度。舍入要按 rm 走，不能一律用宿主的 RNE；
                // 带宽度的舍入要对"超出 float 范围"和"恰好中点"分别处理
                double dv; std::memcpy(&dv, &s.fregs[rs1], 8);
                if (std::isnan(dv)) {
                    // NaN 也要 NaN-box：高半填全 1
                    s.fregs[rd] = 0xFFFFFFFF7FC00000ULL;
                    if (((s.fregs[rs1] >> 52) & 0x7FF) == 0x7FF
                        && (s.fregs[rs1] & 0xFFFFFFFFFFFFFULL) != 0
                        && !(s.fregs[rs1] & 0x8000000000000ULL)) s.fcsr |= 0x10;   // sNaN
                    break;
                }
                {
                    const double fmax = (double) std::numeric_limits<float>::max();
                    const double half_fmax = std::ldexp(1.0, 128) - std::ldexp(1.0, 103);
                    bool neg = std::signbit(dv);
                    double mag = std::fabs(dv);
                    float fv;
                    if (mag > fmax) {
                        // 太大：按 rm 决定是给 inf 还是最大有限值
                        bool to_inf = (xrm == 0) ? (mag >= half_fmax)
                                    : (xrm == 1) ? false
                                    : (xrm == 2) ? neg
                                    : (xrm == 3) ? !neg
                                    : (mag >= half_fmax);
                        fv = to_inf ? (neg ? -INFINITY : INFINITY)
                                    : (neg ? -std::numeric_limits<float>::max()
                                           : std::numeric_limits<float>::max());
                        s.fcsr |= 0x04;   // OF
                        s.fcsr |= 0x01;   // NX
                    } else {
                        fv = (float) dv;   // 宿主 RNE
                        if ((double) fv != dv) {
                            // 非 RNE：先截到目标方向的相邻值
                            double lo = (double) fv;
                            double hi = lo;
                            if (lo > dv) { hi = lo; lo = std::nextafter(lo, 0.0); }
                            else { lo = lo; hi = std::nextafter(lo, neg ? -INFINITY : INFINITY); }
                            double pick = (xrm == 1) ? (neg ? hi : lo)     // RTZ
                                        : (xrm == 2) ? lo                  // RDN
                                        : (xrm == 3) ? hi                  // RUP
                                        : std::fabs(dv - lo) < std::fabs(hi - dv) ? lo
                                          : std::fabs(dv - lo) > std::fabs(hi - dv) ? hi
                                          : (std::fmod(lo, 2.0) == 0.0 ? lo : hi);   // RNE 中点取偶
                            fv = (float) pick;
                            s.fcsr |= 0x01;   // NX
                        }
                    }
                    s.fregs[rd] = 0xFFFFFFFF00000000ULL | (uint64_t)(uint32_t) 0;
                    std::memcpy(&s.fregs[rd], &fv, 4);
                }
            } else if (dst == 1 && src == 0) {
                // FCVT.D.S：单精度 -> 双精度。float 的每个值 double 都能精确表示，不会丢精度
                float fv; uint32_t u = (uint32_t) s.fregs[rs1];
                std::memcpy(&fv, &u, 4);
                if (std::isnan(fv)) { s.fregs[rd] = 0x7FF8000000000000ULL; break; }
                double dv = (double) fv;
                std::memcpy(&s.fregs[rd], &dv, 8);
            } else {
                // 涉及 half 的转换：half→S/D 精确（float/double 都能表示所有 half 值），
                // S/D→half 由 fregWrite 里的 doubleToHalf 按 rm 舍入
                fregWrite(rd, dst, fregRead(rs1, src));
            }
            break;
        }
        // ---- Zfh：fmt=2 的全部算术/比较/转换指令 ----
        if (fmt == 2) {
            if (((s.mstatus >> 13) & 3u) == 0) { takeTrap(s, RV::EXC_II, inst); return; }
            if (xrm > 4) { takeTrap(s, RV::EXC_II, inst); return; }
            const uint32_t op = f7 & ~3u;
            const double ha = fregRead(rs1, 2), hb = fregRead(rs2, 2);
            auto is_snan_h = [](uint64_t u) {
                if ((u >> 16) != 0xFFFFFFFFFFFFULL) return false;
                uint16_t h = (uint16_t) u;
                return ((h >> 10) & 0x1F) == 0x1F && (h & 0x3FF) != 0 && !(h & 0x200);
            };
            const bool hsnan = is_snan_h(s.fregs[rs1])
                            || (op != RV::F7_FSQRT && is_snan_h(s.fregs[rs2]));
            auto setHalf = [&](double v) {
                s.fregs[rd] = 0xFFFFFFFFFFFF0000ULL | (uint64_t) doubleToHalf(v, xrm, s.fcsr);
            };
            // 算术运算产生 NaN 且输入里没有 NaN，说明是无效运算（inf-inf、0*inf、
            // 0/0、inf/inf、sqrt 负数），规范要求置 NV
            auto setHalfOp = [&](double v) {
                if (std::isnan(v)) {
                    const bool invalid = !std::isnan(ha) && (op == RV::F7_FSQRT || !std::isnan(hb));
                    s.fregs[rd] = 0xFFFFFFFFFFFF7E00ULL;
                    if (hsnan || invalid) s.fcsr |= 0x10;
                } else {
                    setHalf(v);
                }
            };
            auto cvtToIntHalf = [&](double a) -> double {
                // FCVT.W/WU/L/LU.H：先按 rm 舍入到整数，再按目标宽度饱和
                switch (xrm) {
                case 1: return std::trunc(a);
                case 2: return std::floor(a);
                case 3: return std::ceil(a);
                case 4: { double t = std::trunc(a), fr = a - t; if (std::fabs(fr) >= 0.5) t += (a > 0 ? 1.0 : -1.0); return t; }
                default: {
                    double fl = std::floor(a), ce = std::ceil(a);
                    double dlo = a - fl, dhi = ce - a;
                    if (dlo < dhi) return fl;
                    if (dhi < dlo) return ce;
                    return std::fmod(fl, 2.0) == 0.0 ? fl : ce;
                }
                }
            };
            switch (op) {
            case RV::F7_FADD: setHalfOp(ha + hb); break;
            case RV::F7_FSUB: setHalfOp(ha - hb); break;
            case RV::F7_FMUL: setHalfOp(ha * hb); break;
            case RV::F7_FDIV: setHalfOp(ha / hb); break;
            case RV::F7_FSQRT: setHalfOp(std::sqrt(ha)); break;
            case RV::F7_FSGNJ: {
                uint16_t ua = (uint16_t) s.fregs[rs1], ub = (uint16_t) s.fregs[rs2];
                uint16_t sg;
                if (f3 == 0) sg = ub & 0x8000;              // FSGNJ
                else if (f3 == 1) sg = (~ub) & 0x8000;      // FSGNJN
                else if (f3 == 2) sg = (ua ^ ub) & 0x8000;  // FSGNJX
                else { takeTrap(s, RV::EXC_II, inst); return; }
                s.fregs[rd] = 0xFFFFFFFFFFFF0000ULL | (uint64_t)((ua & 0x7FFFu) | sg);
                break;
            }
            case RV::F7_FMINMAX: {
                if (std::isnan(ha) && std::isnan(hb)) { s.fregs[rd] = 0xFFFFFFFFFFFF7E00ULL; break; }
                if (std::isnan(ha)) { setHalf(hb); if (hsnan) s.fcsr |= 0x10; break; }
                if (std::isnan(hb)) { setHalf(ha); if (hsnan) s.fcsr |= 0x10; break; }
                // ±0 相等：fmin 取负零、fmax 取正零（IEEE 754 规定 -0 < +0）
                if (f3 == 0) setHalf(ha == hb ? (std::signbit(ha) ? ha : hb) : std::fmin(ha, hb));
                else if (f3 == 1) setHalf(ha == hb ? (std::signbit(ha) ? hb : ha) : std::fmax(ha, hb));
                else { takeTrap(s, RV::EXC_II, inst); return; }
                break;
            }
            case RV::F7_FCMP: {
                if (f3 >= 3) { takeTrap(s, RV::EXC_II, inst); return; }
                uint32_t r = 0;
                if (std::isnan(ha) || std::isnan(hb)) { if (f3 != 2 || hsnan) s.fcsr |= 0x10; }
                else if (f3 == 2) r = (ha == hb) ? 1u : 0u;
                else if (f3 == 1) r = (ha < hb) ? 1u : 0u;
                else r = (ha <= hb) ? 1u : 0u;
                s.regs[rd] = r;
                break;
            }
            case RV::F7_FCVT_W_S: {
                if (rs2 >= 4 || (rs2 >= 2 && XLEN != 64)) { takeTrap(s, RV::EXC_II, inst); return; }
                double r = cvtToIntHalf(ha);
                if (std::isnan(r)) { s.fcsr |= 0x10; }
                // 截掉小数部分（或舍入改变数值）就是不精确
                if (std::isfinite(ha) && std::isfinite(r) && r != ha) s.fcsr |= 0x01;
                if (rs2 == 0) {
                    if (std::isnan(r) || r >= 2147483648.0) { s.regs[rd] = (XLen)(SXLen)0x7FFFFFFF; s.fcsr |= 0x10; }
                    else if (r < -2147483648.0) { s.regs[rd] = (XLen)(SXLen)(int32_t)0x80000000; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen)(SXLen)(int32_t) r;
                } else if (rs2 == 1) {
                    if (std::isnan(r) || r >= 4294967296.0) { s.regs[rd] = (XLen)(SXLen)(int32_t)(uint32_t)0xFFFFFFFFu; s.fcsr |= 0x10; }
                    else if (r < 0.0) { s.regs[rd] = 0; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen)(SXLen)(int32_t)(uint32_t) r;
                } else if (rs2 == 2) {
                    if (std::isnan(r) || r >= 9223372036854775808.0) { s.regs[rd] = (XLen)0x7FFFFFFFFFFFFFFFULL; s.fcsr |= 0x10; }
                    else if (r < -9223372036854775808.0) { s.regs[rd] = (XLen)0x8000000000000000ULL; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen)(SXLen) r;
                } else {
                    if (std::isnan(r) || r >= 18446744073709551616.0) { s.regs[rd] = (XLen)0xFFFFFFFFFFFFFFFFULL; s.fcsr |= 0x10; }
                    else if (r < 0.0) { s.regs[rd] = 0; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen) r;
                }
                break;
            }
            case RV::F7_FCVT_S_W: {
                if (rs2 >= 4 || (rs2 >= 2 && XLEN != 64)) { takeTrap(s, RV::EXC_II, inst); return; }
                double d;
                if (rs2 == 0) d = (double)(int32_t) v1;
                else if (rs2 == 1) d = (double)(uint32_t) v1;
                else if (rs2 == 2) d = (double)(int64_t) v1;
                else d = (double)(uint64_t) v1;
                setHalf(d);
                break;
            }
            case RV::F7_FMV_X_W:
                if (f3 == 0) {
                    // FMV.X.H：把低 16 位符号扩展到 XLEN
                    s.regs[rd] = (XLen)(SXLen)(int16_t)(uint16_t) s.fregs[rs1];
                } else if (f3 == 1) {
                    uint16_t u = (uint16_t) s.fregs[rs1], cls = 0;
                    const bool sign = (u >> 15) != 0;
                    const uint32_t e = (u >> 10) & 0x1F, m = u & 0x3FF;
                    if (e == 0x1F && m == 0) cls = sign ? (1u << 0) : (1u << 7);
                    else if (e == 0x1F) cls = (m & 0x200) ? (1u << 9) : (1u << 8);
                    else if (e == 0 && m == 0) cls = sign ? (1u << 3) : (1u << 4);
                    else if (e == 0) cls = sign ? (1u << 2) : (1u << 5);
                    else cls = sign ? (1u << 1) : (1u << 6);
                    s.regs[rd] = cls;
                } else {
                    takeTrap(s, RV::EXC_II, inst); return;
                }
                break;
            case RV::F7_FMV_W_X:
                if (f3 != 0) { takeTrap(s, RV::EXC_II, inst); return; }
                // FMV.H.X：低 16 位进 f 寄存器，高 48 位填 1（NaN-boxing）
                s.fregs[rd] = 0xFFFFFFFFFFFF0000ULL | (uint64_t)(uint16_t) v1;
                break;
            default:
                takeTrap(s, RV::EXC_II, inst); return;
            }
            break;
        }
        if (fmt == 3) { takeTrap(s, RV::EXC_II, inst); return; } // Q 扩展未实现
        const bool is_d = (fmt == 1);
        // D 扩展与 XLEN 无关：架构上 f 寄存器始终是 64 位宽，RV32 也有 D
        if (is_d) {
            if (((s.mstatus >> 13) & 3u) == 0) { takeTrap(s, RV::EXC_II, inst); return; }
            auto u2d = [](uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; };
            auto d2u = [](double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; };
            const uint32_t op = f7 & ~1u;   // 去掉 fmt 位，得到与 F 一致的操作码
            const uint32_t eff_rm = (f3 == 7) ? ((s.fcsr >> 5) & 7u) : f3;
            double da = u2d(s.fregs[rs1]), db = u2d(s.fregs[rs2]);
            auto is_snan_d = [](uint64_t u) {
                return ((u >> 52) & 0x7FF) == 0x7FF && (u & 0xFFFFFFFFFFFFFULL) != 0
                    && !(u & 0x8000000000000ULL);
            };
            const bool dsnan = is_snan_d(s.fregs[rs1])
                            || (op != RV::F7_FSQRT && is_snan_d(s.fregs[rs2]));

            // 按 rm 把精确值舍入成 double。宿主默认就是 RNE，直接算即可；
            // 其余模式用 nearbyint 那样的思路手动修正，避免依赖宿主的浮点环境
            auto round_to_double = [&](double exact) -> double {
                switch (eff_rm) {
                case 0: return exact;                                  // RNE：宿主默认
                case 1: return std::trunc(exact);                       // RTZ
                case 2: return std::floor(exact);                       // RDN
                case 3: return std::ceil(exact);                        // RUP
                default: {                                              // RMM：中点远离 0
                    double t = std::trunc(exact), f = exact - t;
                    if (std::fabs(f) >= 0.5) t += (exact > 0 ? 1.0 : -1.0);
                    return t;
                }
                }
            };
            // FCVT 要把浮点舍入成整数，语义和"舍入到 double 精度"完全不同：
            // 前者要处理小数部分并按 rm 选整，后者在 double 上本来就是恒等
            auto round_to_int = [&](double v) -> double {
                switch (eff_rm) {
                case 0: {   // RNE：中点取偶
                    double fl = std::floor(v), ce = std::ceil(v);
                    double dlo = v - fl, dhi = ce - v;
                    if (dlo < dhi) return fl;
                    if (dhi < dlo) return ce;
                    return (std::fmod(fl, 2.0) == 0.0) ? fl : ce;
                }
                case 1: return std::trunc(v);   // RTZ
                case 2: return std::floor(v);   // RDN
                case 3: return std::ceil(v);    // RUP
                default: {  // RMM：中点远离 0
                    double t = std::trunc(v), f = v - t;
                    if (std::fabs(f) > 0.5 || (std::fabs(f) == 0.5)) t += (v > 0 ? 1.0 : -1.0);
                    return t;
                }
                }
            };
            auto setd = [&](double v) { s.fregs[rd] = d2u(v); };
            // 双精度没有更宽的宿主类型可用（long double 在 MSVC/clang-cl 上也是 64 位），
            // 所以"结果是否不精确"不能靠比较舍入前后——exact 本身已经是舍入过的值。
            // 这里用 TwoSum / 精确余数来判定：err != 0 就说明真实结果无法用 double 表示
            auto inexact_add = [](double x, double y) -> bool {
                double s = x + y;
                double bb = s - x;
                double err = (x - (s - bb)) + (y - bb);   // Kahan TwoSum 的误差项
                return err != 0.0;
            };
            auto inexact_mul = [](double x, double y) -> bool {
                double p = x * y;
                if (!std::isfinite(p) || x == 0.0 || y == 0.0) return false;
                return std::fma(x, y, -p) != 0.0;         // fma 给出精确余项
            };
            auto inexact_div = [](double x, double y) -> bool {
                double q = x / y;
                if (!std::isfinite(q) || y == 0.0) return false;
                return std::fma(-q, y, x) != 0.0;         // x - q*y 的精确余项
            };
            auto setd_flags = [&](double exact, int inexact_kind = 0, double ox = 0.0, double oy = 0.0) {
                if (std::isnan(exact)) {
                    s.fregs[rd] = 0x7FF8000000000000ULL;   // canonical NaN
                    if (dsnan || (!std::isnan(da) && !std::isnan(db))) s.fcsr |= 0x10;
                    return;
                }
                double r = std::isinf(exact) ? exact : round_to_double(exact);
                setd(r);
                if (std::isinf(r)) {
                    // 有限输入算出无穷才是溢出；exact 本身可能已是 inf，不能用它判断
                    if (std::isfinite(ox) && std::isfinite(oy)) { s.fcsr |= 0x04; s.fcsr |= 0x01; }
                    return;
                }
                // 粗判：舍入前后不同一定不精确；相同则再用精确余项判一次
                bool nx = (r != exact);
                if (!nx && inexact_kind == 1) nx = inexact_add(ox, oy);
                else if (!nx && inexact_kind == 2) nx = inexact_mul(ox, oy);
                else if (!nx && inexact_kind == 3) nx = inexact_div(ox, oy);
                if (nx) {
                    s.fcsr |= 0x01;
                    if (exact != 0.0 && std::fabs(r) < std::numeric_limits<double>::min())
                        s.fcsr |= 0x02;                          // UF
                }
            };

            switch (op) {
            case RV::F7_FADD: setd_flags(da + db, 1, da, db); break;
            case RV::F7_FSUB: setd_flags(da - db, 1, da, -db); break;
            case RV::F7_FMUL: setd_flags(da * db, 2, da, db); break;
            case RV::F7_FDIV:
                if (db == 0.0) {
                    if (da == 0.0) { s.fregs[rd] = 0x7FF8000000000000ULL; s.fcsr |= 0x10; }
                    else { setd(std::signbit(da) != std::signbit(db) ? -INFINITY : INFINITY); s.fcsr |= 0x08; }
                } else setd_flags(da / db, 3, da, db);
                break;
            case RV::F7_FSQRT:
                if (da < 0.0) { s.fregs[rd] = 0x7FF8000000000000ULL; s.fcsr |= 0x10; }
                else if (std::isnan(da)) { s.fregs[rd] = 0x7FF8000000000000ULL; if (dsnan) s.fcsr |= 0x10; }
                else {
                    // sqrt 的结果本身就是舍入后的值，拿它当"精确值"比较永远相等，
                    // NX 就永远置不上。平方回去比才能判断是否精确
                    double r0 = std::sqrt(da);
                    setd(r0);
                    // 不能用 r0*r0 != da 判断：舍入会让两边相等、把不精确误判成精确。
                    // fma(r0,r0,-da) 给出 r0*r0 - da 的精确值，非零才说明 sqrt 真的不精确
                    if (std::fma(r0, r0, -da) != 0.0) s.fcsr |= 0x01;   // NX
                }
                break;
            case RV::F7_FSGNJ: {
                uint64_t sign = 0;
                if (f3 == 0) sign = s.fregs[rs2] & 0x8000000000000000ULL;        // FSGNJ.D
                else if (f3 == 1) sign = (~s.fregs[rs2]) & 0x8000000000000000ULL; // FSGNJN.D
                else { sign = (s.fregs[rs1] ^ s.fregs[rs2]) & 0x8000000000000000ULL; } // FSGNJX.D
                if (f3 > 2) { takeTrap(s, RV::EXC_II, inst); return; }
                s.fregs[rd] = (s.fregs[rs1] & 0x7FFFFFFFFFFFFFFFULL) | sign;
                break;
            }
            case RV::F7_FMINMAX: {
                if (f3 > 1) { takeTrap(s, RV::EXC_II, inst); return; }
                if (std::isnan(da) && std::isnan(db)) { s.fregs[rd] = 0x7FF8000000000000ULL; break; }
                if (std::isnan(da)) { setd(db); if (dsnan) s.fcsr |= 0x10; break; }
                if (std::isnan(db)) { setd(da); if (dsnan) s.fcsr |= 0x10; break; }
                if (da == 0.0 && db == 0.0) {   // ±0 时按符号选，min 取 -0、max 取 +0
                    bool an = std::signbit(da), bn = std::signbit(db);
                    setd((f3 == 0) ? (an || bn ? -0.0 : 0.0) : (an && bn ? -0.0 : 0.0));
                    break;
                }
                setd((f3 == 0) ? (da < db ? da : db) : (da > db ? da : db));
                break;
            }
            case RV::F7_FCMP: {
                if (f3 > 2) { takeTrap(s, RV::EXC_II, inst); return; }
                if (std::isnan(da) || std::isnan(db)) {
                    if (f3 != 2 || dsnan) s.fcsr |= 0x10;   // FEQ 只对 sNaN 报
                    s.regs[rd] = 0;
                } else if (f3 == 2) s.regs[rd] = (da == db) ? 1 : 0;
                else if (f3 == 1) s.regs[rd] = (da < db) ? 1 : 0;
                else s.regs[rd] = (da <= db) ? 1 : 0;
                break;
            }
            case RV::F7_FCVT_W_S: {
                // rs2 选目标宽度：0=W 1=WU 2=L 3=LU
                if (rs2 >= 4) { takeTrap(s, RV::EXC_II, inst); return; }
                if (eff_rm > 4) { takeTrap(s, RV::EXC_II, inst); return; }
                double r = round_to_int(da);   // FCVT 是舍入成整数，不是舍入到 double
                if (rs2 == 0) {   // FCVT.W.D：32 位有符号，RV64 下符号扩展到 XLEN
                    if (std::isnan(r) || r >= 2147483648.0) { s.regs[rd] = (XLen)(SXLen)0x7FFFFFFF; s.fcsr |= 0x10; }
                    else if (r < -2147483648.0) { s.regs[rd] = (XLen)(SXLen)(int32_t)0x80000000; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen)(SXLen)(int32_t) r;
                } else if (rs2 == 1) {   // FCVT.WU.D：规范要求同样符号扩展到 XLEN
                    if (std::isnan(r) || r >= 4294967296.0) { s.regs[rd] = (XLen)(SXLen)(int32_t)(uint32_t)0xFFFFFFFFu; s.fcsr |= 0x10; }
                    else if (r < 0.0) { s.regs[rd] = 0; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen)(SXLen)(int32_t)(uint32_t) r;
                } else if (rs2 == 2) {   // FCVT.L.D
                    if (std::isnan(r) || r >= 9223372036854775808.0) { s.regs[rd] = (XLen) 0x7FFFFFFFFFFFFFFFULL; s.fcsr |= 0x10; }
                    else if (r < -9223372036854775808.0) { s.regs[rd] = (XLen) 0x8000000000000000ULL; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen)(SXLen) r;
                } else {                 // FCVT.LU.D
                    if (std::isnan(r) || r >= 18446744073709551616.0) { s.regs[rd] = (XLen) 0xFFFFFFFFFFFFFFFFULL; s.fcsr |= 0x10; }
                    else if (r < 0.0) { s.regs[rd] = 0; s.fcsr |= 0x10; }
                    else s.regs[rd] = (XLen) r;
                }
                // NX 只在没有饱和时才有意义：饱和路径规范只要求 NV，
                // 拿回写值和原值比较会把"饱和"误判成"不精确"
                if (s.fcsr & 0x10) { /* 已置 NV，不再补 NX */ }
                else if (std::isfinite(da) && r != da) s.fcsr |= 0x01;
                break;
            }
            case RV::F7_FCVT_S_W: {
                // rs2 选源宽度：0=W 1=WU 2=L 3=LU
                if (rs2 >= 4) { takeTrap(s, RV::EXC_II, inst); return; }
                if (eff_rm > 4) { takeTrap(s, RV::EXC_II, inst); return; }
                double src;
                if (rs2 == 0) src = (double)(int32_t) v1;
                else if (rs2 == 1) src = (double)(uint32_t) v1;
                else if (rs2 == 2) src = (double)(int64_t) v1;
                else src = (double)(uint64_t) v1;
                double r = round_to_double(src);
                setd(r);
                if (r != src) s.fcsr |= 0x01;
                break;
            }
            case RV::F7_FMV_X_W:
                if (f3 == 0) {         // FMV.X.D：整 64 位搬运，不做任何转换
                    s.regs[rd] = (XLen) s.fregs[rs1];
                } else if (f3 == 1) {  // FCLASS.D
                    uint64_t u = s.fregs[rs1], cls = 0;
                    const bool sign = (u >> 63) != 0;
                    const uint64_t expo = (u >> 52) & 0x7FF, mant = u & 0xFFFFFFFFFFFFFULL;
                    if (expo == 0x7FF) {
                        if (mant == 0) cls = sign ? (1u << 0) : (1u << 7);
                        else cls = (mant & 0x8000000000000ULL) ? (1u << 9) : (1u << 8);
                    } else if (expo == 0) {
                        if (mant == 0) cls = sign ? (1u << 3) : (1u << 4);
                        else cls = sign ? (1u << 2) : (1u << 5);
                    } else cls = sign ? (1u << 1) : (1u << 6);
                    s.regs[rd] = (XLen) cls;
                } else { takeTrap(s, RV::EXC_II, inst); return; }
                break;
            case RV::F7_FMV_W_X:
                if (f3 != 0) { takeTrap(s, RV::EXC_II, inst); return; }
                s.fregs[rd] = (uint64_t) v1;   // FMV.D.X
                break;
            default: takeTrap(s, RV::EXC_II, inst); return;
            }
            break;
        }
        uint32_t flags_before = s.fcsr; // 用于判断本次是否新置了异常标志
        if (((s.mstatus >> 13) & 3u) == 0) { takeTrap(s, RV::EXC_II, inst); return; } // FS=Off 时禁止浮点指令
        // 把操作数当浮点数用时才检查 NaN-box：高 32 位不是全 1 说明这不是合法的
        // 单精度值，按规范整体当作 canonical NaN。FMV.X.W / FCLASS.S 直接读位模式，
        // 不经过这里，因此不受影响
        auto u2f = [](uint64_t v) -> float {
            uint32_t u = (v >> 32) == 0xFFFFFFFFu ? (uint32_t) v : 0x7FC00000u;
            float f; std::memcpy(&f, &u, 4); return f;
        };
        auto f2u = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        float a = u2f(s.fregs[rs1]), b = u2f(s.fregs[rs2]);
        // qNaN 是静默的，只有 signaling NaN 才触发无效运算
        auto is_snan = [](uint32_t u) {
            return ((u >> 23) & 0xFF) == 0xFF && (u & 0x7FFFFFu) != 0 && !(u & 0x400000u);
        };
        // FSQRT 的 rs2 固定为 0，f0 不是它的输入，拿来判 sNaN 会误报 NV
        bool snan_in = is_snan(s.fregs[rs1]) || (f7 != RV::F7_FSQRT && is_snan(s.fregs[rs2]));
        uint32_t rm = f3;
        // 单精度结果写进 64 位寄存器时只改低 32 位：高半必须保持原样。
        // RISC-V 用 NaN-boxing 区分"合法单精度"和"双精度值"，整体赋值会把
        // 高半清零，随后的 fmv.x.d 就读到错的位模式
        auto setf = [&](float v) {
            s.fregs[rd] = 0xFFFFFFFF00000000ULL | (uint64_t) f2u(v);
        };
        // exact 用 double 保存未舍入的结果，据此判断是否不精确（NX）
        auto setf_flags = [&](double exact, bool cancelled_to_zero = false, bool extra_inexact = false) {
            float r = round_to_float(exact, cur_rm, cancelled_to_zero);
            if (std::isnan(r)) {
                // 规范要求产生 canonical NaN，不同平台算出的 NaN 位模式不统一
                s.fregs[rd] = 0xFFFFFFFF00000000ULL | 0x7FC00000u;
                // sNaN 输入本身就是非法运算；其余情况是"非 NaN 输入算出了 NaN"
                if (snan_in || (!std::isnan(a) && !std::isnan(b))) s.fcsr |= 0x10;
                return;
            }
            setf(r);
            if (!std::isfinite(exact)) return;
            if ((double) r != exact || extra_inexact) s.fcsr |= 0x01; // NX
            // OF：舍入成 inf；UF：结果是非规格化数，或舍入成 0 而精确值非 0
            // RTZ/RDN(正)/RUP(负) 在溢出时返回的是 Fmax，isinf 盖不到，
            // 但按 IEEE 754 只要无界指数下的结果超出 Fmax 就要报 OF
            if (std::isinf(r) || std::fabs(exact) >= 0x1p128) s.fcsr |= 0x04;
            else if ((r == 0.0f ? exact != 0.0
                                : std::fabs((double) r) < (double) std::numeric_limits<float>::min())
                     && (double) r != exact)
                s.fcsr |= 0x02;
        };

        // 这五条的 funct3 是舍入模式：5/6 保留、7 取 fcsr.frm。
        // FSGNJ/FMINMAX/FCMP/FCVT/FMV 的 funct3 另有含义，不能套用这条。
        if (f7 == RV::F7_FADD || f7 == RV::F7_FSUB || f7 == RV::F7_FMUL || f7 == RV::F7_FDIV
            || f7 == RV::F7_FSQRT) {
            if ((rm == 7 ? ((s.fcsr >> 5) & 7u) : rm) > 4) { takeTrap(s, RV::EXC_II, inst); return; }
        }
        switch (f7) {
        // a-b 与 a+(-b) 在"精确抵消成零"时按 IEEE 754 要给出有符号零
        // 零与零的相反符号相加也算抵消（如 (+0)+(-0)、(+0)-(+0)），
        // 所以不能用 a != 0 把零排除
        case RV::F7_FADD: {
            // TwoSum：误差项非零说明 double 这一步就丢了低位，
            // 而这种情况 (double)r != exact 检测不出来（exact 本身已经被舍入过）
            double x = (double) a, y = (double) b, sum = x + y;
            double bv = sum - x;
            double err = (x - (sum - bv)) + (y - bv);
            setf_flags(sum, b == -a && (a != 0.0f || std::signbit(a) != std::signbit(b)), err != 0.0);
            break;
        }
        case RV::F7_FSUB: {
            double x = (double) a, y = -(double) b, sum = x + y;
            double bv = sum - x;
            double err = (x - (sum - bv)) + (y - bv);
            setf_flags(sum, b == a && (a != 0.0f || std::signbit(a) == std::signbit(b)), err != 0.0);
            break;
        }
        case RV::F7_FMUL: setf_flags((double)a * (double)b); break;
        case RV::F7_FDIV:
            // DZ 只适用于"有限非零被除数 / 零"；inf/0 的结果是 inf，不是错误
            if (b == 0.0f && std::isfinite(a) && a != 0.0f) s.fcsr |= 0x08;
            setf_flags((double)a / (double)b);
            break;
        case RV::F7_FSQRT:
            if (rs2 != 0) { takeTrap(s, RV::EXC_II, inst); return; } // FSQRT 的 rs2 固定为 0
            if (a < 0.0f) { s.fcsr |= 0x10; setf(std::numeric_limits<float>::quiet_NaN()); }
            else setf_flags(std::sqrt((double)a));
            break;
        case RV::F7_FSGNJ: {
            // 操作数先按 NaN-boxing 取值：高 32 位不是全 1 的寄存器整体当作
            // canonical NaN，再从中取符号位
            auto box = [](uint64_t v) -> uint32_t {
                return (v >> 32) == 0xFFFFFFFFu ? (uint32_t) v : 0x7FC00000u;
            };
            uint32_t ua = box(s.fregs[rs1]), ub = box(s.fregs[rs2]), sign;
            switch (f3) {
            case 0: sign = ub & 0x80000000u; break;          // FSGNJ
            case 1: sign = (~ub) & 0x80000000u; break;       // FSGNJN
            case 2: sign = (ua ^ ub) & 0x80000000u; break;   // FSGNJX
            default: takeTrap(s, RV::EXC_II, inst); return;  // f3 >= 3 是保留编码
            }
            // 只改低 32 位，高半保持（NaN-boxing）
            s.fregs[rd] = 0xFFFFFFFF00000000ULL | (uint64_t)((ua & 0x7FFFFFFFu) | sign);
            break;
        }
        case RV::F7_FMINMAX:
            if (f3 >= 2) { takeTrap(s, RV::EXC_II, inst); return; } // f3 >= 2 是保留编码
            // NaN 参与时返回另一个操作数并置 NV；两个都是 NaN 才返回 canonical NaN
            if (snan_in) s.fcsr |= 0x10;
            if (std::isnan(a) && std::isnan(b)) s.fregs[rd] = 0xFFFFFFFF00000000ULL | 0x7FC00000u;
            else if (std::isnan(a)) setf(b);
            else if (std::isnan(b)) setf(a);
            else if (f3 == 0) {
                if (a == 0.0f && b == 0.0f) s.fregs[rd] = s.fregs[rs1] | s.fregs[rs2]; // -0 优先
                else setf(a < b ? a : b);
            } else {
                // FMAX 对任何两个零都要给 +0（F 扩展 12.2），
                // 但位与对 (-0,-0) 的结果是 -0，所以要先排掉这种组合
                if (a == 0.0f && b == 0.0f) {
                    uint32_t ua = s.fregs[rs1], ub = s.fregs[rs2];
                    s.fregs[rd] = ((ua | ub) == 0x80000000u) ? 0u : (ua & ub);
                } else setf(a > b ? a : b);
            }
            break;
        case RV::F7_FCMP: {
            if (f3 >= 3) { takeTrap(s, RV::EXC_II, inst); return; } // f3 >= 3 是保留编码
            uint32_t r = 0;
            // FLT/FLE 遇任何 NaN 都产生无效运算；FEQ 只对 signaling NaN 报
            if (std::isnan(a) || std::isnan(b)) {
                if (f3 != 2 || snan_in) s.fcsr |= 0x10;
            }
            if (!std::isnan(a) && !std::isnan(b)) {
                if (f3 == 2) r = (a == b) ? 1u : 0u;      // FEQ
                else if (f3 == 1) r = (a < b) ? 1u : 0u;  // FLT
                else r = (a <= b) ? 1u : 0u;              // FLE
            }
            s.regs[rd] = r;
            break;
        }
        case RV::F7_FCVT_W_S: {
            // rs2 选目标宽度：0=W 1=WU 2=L 3=LU（L/LU 只有 RV64 有）
            if (rs2 >= 4 || (rs2 >= 2 && XLEN != 64)) { takeTrap(s, RV::EXC_II, inst); return; }
            // rm=7 表示用 fcsr.frm；5/6 是保留编码，必须报非法指令而不是当成 RNE
            uint32_t eff_rm = rm == 7 ? ((s.fcsr >> 5) & 7u) : rm;
            if (eff_rm > 4) { takeTrap(s, RV::EXC_II, inst); return; }
            float r;
            switch (eff_rm) {
            case 1: r = std::trunc(a); break;   // RTZ
            case 2: r = std::floor(a); break;   // RDN
            case 3: r = std::ceil(a); break;    // RUP
            case 4: r = std::round(a); break;   // RMM
            default: {
                // 不能用 nearbyint：它跑的是当前浮点环境的舍入方向，
                // 宿主或 DLL 调过 fesetround 就会把 RNE 悄悄变成别的模式
                double fl = std::floor(a), ce = std::ceil(a);
                double dlo = a - fl, dhi = ce - a;
                if (dlo < dhi) r = fl;
                else if (dhi < dlo) r = ce;
                else r = (std::fmod(fl, 2.0) == 0.0 ? fl : ce); // 中点取偶
                break;
            }
            }
            if (std::isfinite(a) && std::isfinite(r) && r != a) s.fcsr |= 0x01; // 截掉小数部分即不精确
            if (rs2 == 0) { // FCVT.W.S
                // RV64 的 FCVT.W.S 要把 32 位结果符号扩展到 XLEN，负值不能零扩展
                XLen out;
                if (std::isnan(r) || r >= 2147483648.0f) { out = (XLen)(SXLen)0x7FFFFFFF; s.fcsr |= 0x10; }
                else if (r < -2147483648.0f) { out = (XLen)(SXLen)(int32_t)0x80000000; s.fcsr |= 0x10; }
                else out = (XLen)(SXLen)(int32_t)r;
                s.regs[rd] = out;
            } else if (rs2 == 1) { // FCVT.WU.S
                // 规范：RV64 下 FCVT.W.S 和 FCVT.WU.S 的 32 位结果都符号扩展到 XLEN，
                // 零扩展的只有 LWU。这一点被误改过两次（RV64 官方 fcvt_w 用例会红）
                if (std::isnan(r) || r >= 4294967296.0f) { s.regs[rd] = (XLen)(SXLen)(int32_t)(uint32_t) 0xFFFFFFFFu; s.fcsr |= 0x10; }
                // r 已经是舍入后的整数值，只要为负就饱和到 0。
                // 写成 <= -1 会漏掉 (-1,0) 区间，那里 (uint32_t)负数是 UB
                else if (r < 0.0f) { s.regs[rd] = 0; s.fcsr |= 0x10; }
                else s.regs[rd] = (XLen)(SXLen)(int32_t)(uint32_t) r;
            } else if (rs2 == 2) { // FCVT.L.S：转 64 位有符号，饱和边界是 2^63
                if (std::isnan(r) || r >= 9223372036854775808.0f) { s.regs[rd] = (XLen) 0x7FFFFFFFFFFFFFFFULL; s.fcsr |= 0x10; }
                else if (r < -9223372036854775808.0f) { s.regs[rd] = (XLen) 0x8000000000000000ULL; s.fcsr |= 0x10; }
                else s.regs[rd] = (XLen)(SXLen) r;
            } else { // FCVT.LU.S：转 64 位无符号，负值饱和到 0
                if (std::isnan(r) || r >= 18446744073709551616.0f) { s.regs[rd] = (XLen) 0xFFFFFFFFFFFFFFFFULL; s.fcsr |= 0x10; }
                else if (r < 0.0f) { s.regs[rd] = 0; s.fcsr |= 0x10; }
                else s.regs[rd] = (XLen) r;
            }
            break;
        }
        case RV::F7_FCVT_S_W: {
            // rs2 选源宽度：0=W 1=WU 2=L 3=LU（L/LU 只有 RV64 有）
            if (rs2 >= 4 || (rs2 >= 2 && XLEN != 64)) { takeTrap(s, RV::EXC_II, inst); return; }
            if ((rm == 7 ? ((s.fcsr >> 5) & 7u) : rm) > 4) { takeTrap(s, RV::EXC_II, inst); return; } // 5/6 保留
            // uint32 → float 会丢精度（如 0xFFFFFFFF），规范要求此时设 NX，并且按 rm 舍入
            // W/WU 只看低 32 位；L/LU 用整个 64 位
            double d;
            if (rs2 == 0) d = (double)(int32_t) v1;
            else if (rs2 == 1) d = (double)(uint32_t) v1;
            else if (rs2 == 2) d = (double)(int64_t) v1;
            else d = (double)(uint64_t) v1;
            float f = round_to_float(d, cur_rm);
            if ((double) f != d) s.fcsr |= 0x01;
            setf(f);
            break;
        }
        case RV::F7_FMV_X_W:
            if (f3 == 0) { // FMV.X.W
                // RV64 下要把低 32 位符号扩展到 XLEN，否则负的位模式会变成大正数
                s.regs[rd] = (XLen)(SXLen)(int32_t) s.fregs[rs1];
            } else if (f3 == 1) { // FCLASS.S（f3 >= 2 是保留编码）
                uint32_t u = s.fregs[rs1], cls = 0;
                bool sign = (u >> 31) != 0;
                uint32_t exp = (u >> 23) & 0xFF, frac = u & 0x7FFFFF;
                if (exp == 0xFF && frac == 0) cls = sign ? (1u << 0) : (1u << 7); // -inf / +inf
                else if (exp == 0xFF) cls = (frac & 0x400000u) ? (1u << 9) : (1u << 8); // quiet / signaling NaN
                else if (exp == 0 && frac == 0) cls = sign ? (1u << 3) : (1u << 4); // -0 / +0
                else if (exp == 0) cls = sign ? (1u << 2) : (1u << 5);              // 非规格化
                else cls = sign ? (1u << 1) : (1u << 6);                            // 规格化
                s.regs[rd] = cls;
            } else {
                takeTrap(s, RV::EXC_II, inst); return;
            }
            break;
        case RV::F7_FMV_W_X:
            if (f3 != 0) { takeTrap(s, RV::EXC_II, inst); return; } // funct3 固定为 0
            // FMV.W.X 写入的是单精度位模式，高半要填全 1（NaN-boxing）。
            // 直接写整个 XLEN 值会让高半变成 0，随后的单精度读取就判成 NaN
            s.fregs[rd] = 0xFFFFFFFF00000000ULL | (uint64_t)((uint32_t) v1);
            break;
        default:
            takeTrap(s, RV::EXC_II, inst); return;
        }
        // FS=Dirty 只在浮点状态确实被修改时设置：FMV.X.W/FCLASS 只读 freg，
        // FCMP 只有在置了 NV 时才算修改，否则上下文切换会白白多保存一次 FPU 状态
        // FCVT.W.S/WU.S 只读 freg、只在产生 NX/NV 时才动浮点状态；
        // 精确转换若也置 Dirty，上下文切换会白白多保存一次 FPU。
        // 判断必须用"本次是否新置位"：用累计的 fcsr 会把之前就存在的
        // NX/NV 算进来，精确的指令也会被当成修改了浮点状态
        uint32_t flags_new = s.fcsr & ~flags_before;
        bool fcvt_to_int = (f7 == RV::F7_FCVT_W_S);
        if (f7 != RV::F7_FMV_X_W
            && (!fcvt_to_int || (flags_new & 0x11))
            && (f7 != RV::F7_FCMP || (flags_new & 0x10)))
            s.mstatus |= (3u << 13);
        break;
    }

    case RV::MISC_MEM:
        // f3=0 是 FENCE（本核顺序执行，无需动作），f3=1 是 FENCE.I，其余是保留编码
        if (f3 > 1) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        if (f3 == 1) {
            for (auto &l : s.icache) l.valid = false;
        }
        break;

    // ---- SYSTEM ----
    case RV::SYSTEM: {
        if (f3 == 0) {
            uint32_t f12=(inst>>20)&RV::M_F12;
            switch (f12) {
            case RV::F12_ECALL:
                if (s.priv == RV::PRIV_U) takeTrap(s, RV::EXC_ECALL_U, 0);
                else if (s.priv == RV::PRIV_S) takeTrap(s, RV::EXC_ECALL_S, 0);
                else takeTrap(s, RV::EXC_ECALL_M, 0);
                npc = s.pc; // takeTrap 可能已修改 PC，不要再改
                break;
            case RV::F12_EBREAK: takeTrap(s, RV::EXC_BP, s.pc); npc = s.pc; break;
            case RV::F12_MRET: {
                if (s.priv != RV::PRIV_M) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                uint8_t mpp=(s.mstatus>>11)&3;
                s.priv = (mpp==0)?RV::PRIV_U:(mpp==1)?RV::PRIV_S:RV::PRIV_M;
                s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_MIE) | ((s.mstatus & RV::MSTATUS_MPIE)>>4);
                s.mstatus |= RV::MSTATUS_MPIE; s.mstatus &= ~(XLen)RV::MSTATUS_MPP;
                // MPP≠M 时规范要求清 MPRV：留着的话返回后 M 态的数据访存会按旧特权级
                // 翻译，地址空间和预期不一致
                if (mpp != RV::PRIV_M) s.mstatus &= ~(XLen)RV::MSTATUS_MPRV;
                npc = s.mepc;
                break;
            }
            case RV::F12_SRET: {
                // TSR=1 时 S 态执行 SRET 是非法指令（M 态不受限）
                if (s.priv == RV::PRIV_S && (s.mstatus & RV::MSTATUS_TSR)) [[unlikely]]
                { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                if (s.priv == RV::PRIV_U) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                s.priv = (s.mstatus & RV::MSTATUS_SPP) ? RV::PRIV_S : RV::PRIV_U;
                s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_SIE) | ((s.mstatus & RV::MSTATUS_SPIE)>>4);
                s.mstatus |= RV::MSTATUS_SPIE; s.mstatus &= ~(XLen)RV::MSTATUS_SPP;
                // SRET 的目标只能是 S/U，都低于 M，规范要求这种 xRET 清 MPRV
                s.mstatus &= ~(XLen)RV::MSTATUS_MPRV;
                npc = s.sepc;
                break;
            }
            case RV::F12_WFI:
                // WFI 是 S 级指令，U 态无条件非法；S 态还要看 TW
                if (s.priv == RV::PRIV_U) [[unlikely]]
                { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                if (s.priv == RV::PRIV_S && (s.mstatus & RV::MSTATUS_TW)) [[unlikely]]
                { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                // 有中断挂起就继续执行；否则停在这里等中断唤醒
                if (!(s.mip & s.mie) && s.mie != 0) {
                    s.wfi_halted = true;
                    s.phase = Phase::HALTED; // 停止取指，等中断唤醒
                    // PC 照常前进：中断返回若落回 WFI 本身，会再停一次机，
                    // 而这时中断源已经处理掉，就再也醒不过来了
                    npc = s.pc + s.inst_len;
                }
                break;
            default:
                // SFENCE.VMA: funct7=0x09, 刷新全部 TLB
                if (f7 == 0x09) {
                    // SFENCE.VMA 是 S 级指令，U 态执行属于非法指令；
                    // TVM=1 时 S 态执行同样非法（只有 M 态可以）
                    if (s.priv == RV::PRIV_U || (s.priv == RV::PRIV_S && (s.mstatus & RV::MSTATUS_TVM)))
                    { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                    for (auto &e : s.tlb) e.valid = false;
                    s.l1_cache_valid = false; // 页表可能被改过，L1 缓存同样失效
                } else {
                    takeTrap(s, RV::EXC_II, inst); npc = s.pc;
                }
                break;
            }
        } else {
            if (f3 == 4) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; } // funct3=4 未定义
            uint32_t csr_addr=(inst>>20)&RV::M_F12; XLen csr_val=csrRead(s, csr_addr);
            // 访问未实现的 CSR、或写只读 CSR（csr[11:10]==3 且确实是写类指令）都是非法指令
            // 只有 CSRRW/CSRRWI 一定写；CSRRS/CSRRC/CSRRSI/CSRRCI 在源操作数为 0 时只是读
            bool csr_write = (f3 == RV::F3_CSRRW || f3 == RV::F3_CSRRWI) || (rs1 != 0);
            if (!csrImplemented<XLEN>(csr_addr) || (((csr_addr >> 10) & 3) == 3 && csr_write)) [[unlikely]]
            { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
            // TVM=1 时 S 态读写 satp 是非法指令
            if (csr_addr == RV::CSR_SATP && s.priv == RV::PRIV_S && (s.mstatus & RV::MSTATUS_TVM)) [[unlikely]]
            { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
            // FS=Off 时访问浮点 CSR 同样是非法指令（读也不行）
            if ((csr_addr == RV::CSR_FFLAGS || csr_addr == RV::CSR_FRM || csr_addr == RV::CSR_FCSR)
                && ((s.mstatus >> 13) & 3u) == 0) [[unlikely]]
            { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
            XLen wval = (f3>=RV::F3_CSRRW&&f3<=RV::F3_CSRRC) ? v1 : (XLen)rs1;
            // 检查 CSR 访问权限 (最低两位 = 读写权限位)
            if ((csr_addr>>8)&3) { uint8_t min_priv=(uint8_t)((csr_addr>>8)&3); if(s.priv<min_priv) [[unlikely]] { takeTrap(s,RV::EXC_II,inst); npc=s.pc; break; }}
            // 计数器 CSR 在 S 态以下还要过 mcounteren 的对应位：CY(0)/TM(1)/IR(2)
            if (s.priv < RV::PRIV_M) {
                int cbit = (csr_addr == RV::CSR_CYCLE || csr_addr == RV::CSR_CYCLEH) ? 0
                           : (csr_addr == RV::CSR_TIME || csr_addr == RV::CSR_TIMEH) ? 1
                           : (csr_addr == RV::CSR_INSTRET || csr_addr == RV::CSR_INSTRETH) ? 2
                                                                     : -1;
                if (cbit >= 0 && !((s.mcounteren >> cbit) & 1)) [[unlikely]]
                { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                // U 态还要再过一道 scounteren：S 态软件靠它单独关掉 U 态的计数器访问
                if (cbit >= 0 && s.priv == RV::PRIV_U && !((s.scounteren >> cbit) & 1)) [[unlikely]]
                { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
            }
            switch(f3){
            case RV::F3_CSRRW: case RV::F3_CSRRWI: csrWrite(s, csr_addr, wval); break;                              // CSR 写入不依赖 rd
            case RV::F3_CSRRS: case RV::F3_CSRRSI: if(rs1!=0) csrWrite(s, csr_addr, csr_val|wval); break;           // rs1=0 时不写 CSR
            case RV::F3_CSRRC: case RV::F3_CSRRCI: if(rs1!=0) csrWrite(s, csr_addr, csr_val&~wval); break;
            }
            s.regs[rd]=csr_val;
        }
        break;
    }

    default: [[unlikely]] takeTrap(s, RV::EXC_II, inst); npc = s.pc; break;
    }

    // ---- PC 更新 ----
    s.regs[0]=0; s.pc=npc;
    // 规范允许"是否把 trap 的指令计入"由实现决定；这里凡是走到尾部收尾的都计，
    // 少数提前 return 的错误路径不计，属于同一取舍下的正常差异
    // 规范：写入 minstret 的值就是紧接着那条指令读到的值，
    // 所以写指令本身不能递增
    if (s.instret_written) s.instret_written = false; else s.instret++;
    if (!need_mem) { wb_adr=s.pc; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=true; }
}

// ============================================================
// CSR 读写
// ============================================================
// 已实现的 CSR 白名单：不在这里的地址一律当非法指令处理，避免软件误以为读写生效
template<int XLEN> requires SupportedXlen<XLEN>
static bool csrImplemented(uint32_t addr) {
    // PMP 配置/地址寄存器：本核不实现保护，但寄存器必须存在，软件会写 0 来关闭它
    // pmpcfg0-3 是 0x3A0-0x3A3，pmpaddr0-15 是 0x3B0-0x3BF；
    // 0x3A4-0x3AF 和 0x3C0-0x3EF 是保留地址，访问必须报非法指令，
    // 否则软件能写进去并读回 0，误以为写成功了
    // RV64 的 pmpcfg 每项占 8 字节，只有 pmpcfg0/pmpcfg2（奇数号是 RV32 的布局）
    if (addr >= RV::CSR_PMPCFG0 && addr <= RV::CSR_PMPCFG3) return (XLEN == 32) || ((addr & 1) == 0);
    if (addr >= RV::CSR_PMPADDR0 && addr <= RV::CSR_PMPADDR15) return true;
    switch (addr) {
    case RV::CSR_FFLAGS: case RV::CSR_FRM: case RV::CSR_FCSR:
    case RV::CSR_MCOUNTEREN: case RV::CSR_SCOUNTEREN:
    // mstatush 在 RV32 上只有 SBE/MBE/GVA/MPV 等位。本核是大端不支持、也没有
    // 客户机虚拟化，这些位全部硬连 0：读回 0，写入忽略，即下面的默认分支行为
    case RV::CSR_MCYCLE: case RV::CSR_MINSTRET:
    case RV::CSR_CYCLE: case RV::CSR_TIME: case RV::CSR_INSTRET:
    case RV::CSR_MVENDORID: case RV::CSR_MARCHID: case RV::CSR_MIMPID: case RV::CSR_MHARTID:
    case RV::CSR_MNSTATUS:
        return true;
    // 下面这些只在 RV32 存在：RV64 的计数器是完整的 64 位，没有高半 CSR，
    // mstatush 也只在 RV32 有意义
    case RV::CSR_MSTATUSH: case RV::CSR_MCYCLEH: case RV::CSR_MINSTRETH:
    case RV::CSR_CYCLEH: case RV::CSR_TIMEH: case RV::CSR_INSTRETH:
        return (XLEN == 32);
    case RV::CSR_SSTATUS: case RV::CSR_SIE: case RV::CSR_STVEC: case RV::CSR_SSCRATCH:
    case RV::CSR_SEPC: case RV::CSR_SCAUSE: case RV::CSR_STVAL: case RV::CSR_SIP: case RV::CSR_SATP:
    case RV::CSR_MSTATUS: case RV::CSR_MISA: case RV::CSR_MEDELEG: case RV::CSR_MIDELEG:
    case RV::CSR_MIE: case RV::CSR_MTVEC: case RV::CSR_MSCRATCH: case RV::CSR_MEPC:
    case RV::CSR_MCAUSE: case RV::CSR_MTVAL: case RV::CSR_MIP:
        return true;
    // Debug 触发模块的 CSR（mcontrol 子集）
    case RV::CSR_TSELECT: case RV::CSR_TDATA1: case RV::CSR_TDATA2:
    case RV::CSR_TDATA3: case RV::CSR_TINFO: case RV::CSR_TCONTROL:
        return true;
    default:
        return false;
    }
}

template<int XLEN> requires SupportedXlen<XLEN>
XLenOf<XLEN> RiscVCPU<XLEN>::csrRead(const CpuState &s, uint32_t addr) {
    if (addr >= RV::CSR_PMPCFG0 && addr <= RV::CSR_PMPCFG3) return s.pmpcfg[addr - RV::CSR_PMPCFG0];
    if (addr >= RV::CSR_PMPADDR0 && addr <= RV::CSR_PMPADDR15)
        return s.pmpaddr[addr - RV::CSR_PMPADDR0];
    switch (addr) {
    // cycle/instret 与 M 态的 mcycle/minstret 是同一对计数器，只是可见性不同
    // RV64 的 cycle/mcycle 是完整的 64 位 CSR；RV32 才拆成低/高两个
    case RV::CSR_MCYCLE: case RV::CSR_CYCLE: return (XLen)((XLEN == 64) ? s.cycles : (s.cycles & 0xFFFFFFFFu));
    case RV::CSR_MCYCLEH: case RV::CSR_CYCLEH: return (uint32_t) (s.cycles >> 32);
    // time 本应来自 CLINT 的 mtime，这里用 cycle 近似：本核时钟与 mtime 同源，
    // 恒返回 0 会让依赖 rdtime 的超时逻辑静默失效
    case RV::CSR_TIME: return (XLen)((XLEN == 64) ? s.cycles : (s.cycles & 0xFFFFFFFFu));
    case RV::CSR_TIMEH: return (uint32_t) (s.cycles >> 32);
    case RV::CSR_MINSTRET: case RV::CSR_INSTRET: return (XLen)((XLEN == 64) ? s.instret : (s.instret & 0xFFFFFFFFu));
    case RV::CSR_MINSTRETH: case RV::CSR_INSTRETH: return (uint32_t) (s.instret >> 32);
    case RV::CSR_MCOUNTEREN: return s.mcounteren;
    case RV::CSR_SCOUNTEREN: return s.scounteren;
    case RV::CSR_FFLAGS:  return s.fcsr & 0x1Fu;
    case RV::CSR_FRM:     return (s.fcsr >> 5) & 0x7u;
    case RV::CSR_FCSR:    return s.fcsr & 0xFFu;
    // SD 是只读汇总位：FS 或 XS 非 0 时必须为 1，软件靠它判断是否需要保存浮点上下文
    case RV::CSR_MSTATUS: {
        // UXL/SXL 在 RV64 恒为 2（64 位），只读：S/U 态与本核 M 态同宽
        XLen uxl_sxl = 0;
        if constexpr (XLEN == 64) uxl_sxl = ((XLen) 2 << 32) | ((XLen) 2 << 34);
        return s.mstatus | uxl_sxl | ((((s.mstatus >> 13) & 3u) != 0) ? ((XLen) 1 << (XLEN - 1)) : 0u);
    }
    // MXL=1(RV32) | I(8) | M(12) | A(0) | F(5) | S(18) | U(20) —— 与实现的指令集和特权级一一对应
    // MXL 字段必须反映实际位宽：RV32 是 1、RV64 是 2。
    // 后面两个位是 A 和 F 扩展
    case RV::CSR_MISA: {
        // MXL 反映实际位宽：RV32 是 1、RV64 是 2。用 if constexpr 分档，
        // 免得 RV32 实例化里出现 (uint32_t)2 << 62 这种越界移位
        XLen mxl = 0;
        if constexpr (XLEN == 64) mxl = (XLen) 2 << 62;
        else mxl = (XLen) 1 << 30;
        return mxl
               | (1u << 0)   // A
               | (1u << 2)   // C（支持压缩指令，IALIGN=16）
               | (1u << 3)   // D（只有 RV64 实现，RV32 的 D 要 64 位浮点寄存器）
               | (1u << 5)   // F
               | (1u << 8)   // I
               | (1u << 12)  // M
               | (1u << 18)  // S
               | (1u << 20)  // U
               | (1u << 21); // H（Zfh 半精度浮点）
    }
    case RV::CSR_MEDELEG: return s.medeleg;
    case RV::CSR_MIDELEG: return s.mideleg;
    case RV::CSR_MIE:     return s.mie;
    case RV::CSR_MTVEC:   return s.mtvec;
    case RV::CSR_MSCRATCH:return s.mscratch;
    case RV::CSR_MEPC:    return s.mepc;
    case RV::CSR_MCAUSE:  return s.mcause;
    case RV::CSR_MTVAL:   return s.mtval;
    case RV::CSR_MIP:     return s.mip;
    // SIE/SPIE/SPP/FS/XS/SUM/MXR 可读，SD 由 FS/XS 汇总而来，不含 M 级字段
    case RV::CSR_SSTATUS: {
        XLen uxl = 0;
        if constexpr (XLEN == 64) uxl = (XLen) 2 << 32;
        return (s.mstatus & 0x000DE122u) | uxl | ((((s.mstatus >> 13) & 3u) != 0) ? ((XLen) 1 << (XLEN - 1)) : 0u);
    }
    case RV::CSR_SIE:     return s.mie & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI);
    case RV::CSR_STVEC:   return s.stvec;
    case RV::CSR_SSCRATCH:return s.sscratch;
    case RV::CSR_SEPC:    return s.sepc;
    case RV::CSR_SCAUSE:  return s.scause;
    case RV::CSR_STVAL:   return s.stval;
    case RV::CSR_SIP:     return s.mip & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI);
    case RV::CSR_SATP:    return s.satp;
    case RV::CSR_TSELECT: return s.tselect;
    case RV::CSR_TDATA1:  return s.tdata1[s.tselect];
    case RV::CSR_TDATA2:  return s.tdata2[s.tselect];
    case RV::CSR_TDATA3:  return s.tdata3[s.tselect];
    // 支持的触发类型位图：bit2 = mcontrol。tinfo 只读，写它由通用只读检查拦下
    case RV::CSR_TINFO:   return (XLen)(1u << RV::TRIG_TYPE_MCONTROL);
    case RV::CSR_TCONTROL:return s.tcontrol;
    default: return 0;
    }
}
template<int XLEN> requires SupportedXlen<XLEN>
void RiscVCPU<XLEN>::csrWrite(CpuState &s, uint32_t addr, XLenOf<XLEN> val) {
    if (addr >= RV::CSR_PMPCFG0 && addr <= RV::CSR_PMPCFG3) { s.pmpcfg[addr - RV::CSR_PMPCFG0] = val; return; }
    if (addr >= RV::CSR_PMPADDR0 && addr <= RV::CSR_PMPADDR15) { s.pmpaddr[addr - RV::CSR_PMPADDR0] = val; return; }
    switch (addr) {
    // pmpaddr 是 0x3B0..0x3BF，pmpcfg 是 0x3A0..0x3A3
    // mcycle/minstret 是 R/W：软件常用写它们重置计时基准
    // RV32 只有低 32 位可写；RV64 是完整 64 位 CSR，OR 进去会把高半写坏
    case RV::CSR_MCYCLE: s.cycles = (XLEN == 64) ? (uint64_t) val
                                        : ((s.cycles & 0xFFFFFFFF00000000ULL) | (uint64_t) val); break;
    case RV::CSR_MCYCLEH: s.cycles = (s.cycles & 0x00000000FFFFFFFFULL) | ((uint64_t) val << 32); break;
    case RV::CSR_MINSTRET: s.instret_written = true; s.instret = (XLEN == 64) ? (uint64_t) val
                                         : ((s.instret & 0xFFFFFFFF00000000ULL) | (uint64_t) val); break;
    case RV::CSR_MINSTRETH: s.instret_written = true; s.instret = (s.instret & 0x00000000FFFFFFFFULL) | ((uint64_t) val << 32); break;
    case RV::CSR_MCOUNTEREN: s.mcounteren = (uint32_t) val & 7u;
        break;
    case RV::CSR_SCOUNTEREN: s.scounteren = (uint32_t) val & 7u; break; // 只有 CY/TM/IR 三位有定义
    case RV::CSR_FFLAGS:  s.fcsr = (s.fcsr & ~0x1Fu) | (val & 0x1Fu); break;
    case RV::CSR_FRM:     s.fcsr = (s.fcsr & ~0xE0u) | ((val & 0x7u) << 5); break;
    case RV::CSR_FCSR:    s.fcsr = val & 0xFFu; break;
    // 本核没有额外扩展状态、也不支持非字节序：
    //   XS(15:16) 恒 0、UBE(6) 恒 0、bit23 是 WPRI 保留位，都不能写
    case RV::CSR_MSTATUS: s.mstatus = val & (XLen)0x007E79AAu; break; // UXL/SXL 只读，不在掩码里  // 掩码含 SIE/MIE/SPIE/MPIE/SPP/MPP/FS/MPRV/SUM/MXR/TVM/TW/TSR；UBE 与 XS 本核恒 0，故不在掩码内
    // bit 11（M 态 ecall）按规范不能委托，必须硬连 0
    case RV::CSR_MEDELEG: s.medeleg = val & 0xF7FFu; break;
    case RV::CSR_MIDELEG: s.mideleg = val & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI); break;
    case RV::CSR_MIE:     s.mie = val & (RV::IRQ_SSI|RV::IRQ_MSI|RV::IRQ_STI|RV::IRQ_MTI|RV::IRQ_SEI|RV::IRQ_MEI); break;
    case RV::CSR_MTVEC:   s.mtvec = val & ~(XLen)2; break; // 保留 bit0（MODE），只清保留位 bit1
    case RV::CSR_MSCRATCH:s.mscratch = val; break;
    // 支持 C 扩展后 IALIGN=16，返回地址可以是 4n+2，只能清最低位；
    // 按 IALIGN=32 清两位会把 bit1 抹掉，mret 回到错的指令，表现为 trap 死循环
    case RV::CSR_MEPC:    s.mepc = val & ~(XLen)1; break;
    case RV::CSR_MCAUSE:  s.mcause = val; break;
    case RV::CSR_MTVAL:   s.mtval = val; break;
    case RV::CSR_SSTATUS: s.mstatus = (s.mstatus & ~(XLen)0x000C6122u) | (val & (XLen)0x000C6122u); break; // 去掉 XS[1:0]，SD(bit31) 只读
    case RV::CSR_SIE:     s.mie = (s.mie & ~(RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)) | (val & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)); break;
    // 软件能改的挂起位只有 SSIP。MSIP/STIP/SEIP 都由外部源驱动
    //（CLINT 的 msip/mtimecmp、PLIC 的 SEIP），每个 tick 会被输入信号覆盖，
    // 这里允许写入只会让软件以为写成功了
    case RV::CSR_MIP:     s.mip = (s.mip & ~RV::IRQ_SSI) | (val & RV::IRQ_SSI); break;
    case RV::CSR_SIP:     s.mip = (s.mip & ~RV::IRQ_SSI) | (val & RV::IRQ_SSI); break; // 只允许改 SSIP
    case RV::CSR_STVEC:   s.stvec = val & ~(XLen)2; break;
    case RV::CSR_SSCRATCH:s.sscratch = val; break;
    // 与 MEPC 同理：有 C 扩展后返回地址可以是 4n+2，只能清 bit0
    case RV::CSR_SEPC:    s.sepc = val & ~(XLen)1; break;
    case RV::CSR_SCAUSE:  s.scause = val; break;
    case RV::CSR_STVAL:   s.stval = val; break;
    case RV::CSR_SATP: {
        // RV32 的 mode 是 bit31（Sv32=1）；RV64 是 bit63:60（Sv39=8）。
        // 只接受已实现的模式：写入 Sv48/Sv57 等未实现的模式会被忽略并保持原值，
        // 而不是把 satp 存下来却按别的页表格式乱翻页表
        bool accept = false;
        if constexpr (XLEN == 64) {
            const uint32_t mode = (uint32_t)(val >> 60);
            accept = (mode == 0 || mode == RV::SATP_MODE_SV39);
        } else {
            const uint32_t mode = val >> 31;
            accept = (mode == 0 || mode == 1);
        }
        if (accept) {
            s.satp = val;
            for (auto &e : s.tlb) e.valid = false;
            s.l1_cache_valid = false; // 换页表后旧的 L1 缓存指向旧根页表
        }
        break;
    }
    // Debug 触发模块：tselect 选槽位，其余按当前槽位读写
    case RV::CSR_TSELECT: s.tselect = (uint32_t)(val & (RV::TRIGGER_NUM - 1)); break;
    case RV::CSR_TDATA1:  s.tdata1[s.tselect] = val; break;
    case RV::CSR_TDATA2:  s.tdata2[s.tselect] = val; break;
    case RV::CSR_TDATA3:  s.tdata3[s.tselect] = val; break;
    // mte(bit3) 控制 M 态触发，其余位保留
    case RV::CSR_TCONTROL: s.tcontrol = (uint32_t)(val & 0x3F); break;
    default: break;
    }
}

// ============================================================
// 中断裁决（考虑委托）
// ============================================================
template<int XLEN> requires SupportedXlen<XLEN>
bool RiscVCPU<XLEN>::takeInterrupt(CpuState &s, XLenOf<XLEN> &trap_cause) {
    uint32_t pending = s.mie & s.mip;
    if (!pending) [[likely]] return false; // 绝大多数时候无中断等待

    // M 级中断不可委托；S 级中断只有 mideleg 置位后才归 S 处理
    uint32_t deleg = s.mideleg & (RV::IRQ_SSI | RV::IRQ_STI | RV::IRQ_SEI);
    uint32_t pend_s = pending & deleg;
    uint32_t pend_m = pending & ~deleg;

    // 全局使能位只约束本级：当前特权级低于目标级时，中断总是使能
    bool m_on = (s.priv < RV::PRIV_M) || (s.mstatus & RV::MSTATUS_MIE);
    // M 态不会被更低优先级的 S 中断打断
    // M 态里挂起的委托中断按 M 态中断处理：直接丢弃的话，M 态常驻时
    // 已委托给 S 的中断永远得不到响应
    bool s_deleg_in_m = (s.priv == RV::PRIV_M);
    bool s_on = s_deleg_in_m || (s.priv < RV::PRIV_S) || (s.mstatus & RV::MSTATUS_SIE);

    // 优先级: MEI > MSI > MTI > SEI > SSI > STI
    if (m_on && pend_m) {
        if (pend_m & RV::IRQ_MEI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 11; return true; }
        if (pend_m & RV::IRQ_MSI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 3; return true; }
        if (pend_m & RV::IRQ_MTI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 7; return true; }
        if (pend_m & RV::IRQ_SEI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 9; return true; }
        if (pend_m & RV::IRQ_SSI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 1; return true; }
        if (pend_m & RV::IRQ_STI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 5; return true; }
    }
    if ((s_deleg_in_m ? m_on : s_on) && pend_s) {
        if (pend_s & RV::IRQ_SEI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 9; return true; }
        if (pend_s & RV::IRQ_SSI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 1; return true; }
        if (pend_s & RV::IRQ_STI) { trap_cause = ((XLen)1 << (XLEN - 1)) | 5; return true; }
    }
    return false;
}

// ============================================================
// 同步异常陷阱入口
// ============================================================
template<int XLEN> requires SupportedXlen<XLEN>
bool RiscVCPU<XLEN>::takeTrap(CpuState &s, XLenOf<XLEN> cause, XLenOf<XLEN> tval) {
    bool is_irq = (cause >> (XLEN - 1)) != 0;
    uint8_t target_priv = RV::PRIV_M;

    // 检查委托
    if (s.priv <= RV::PRIV_S) {
        if (is_irq && (s.mideleg & (1u << (cause & 0x1F)))) target_priv = RV::PRIV_S;
        else if (!is_irq && (s.medeleg & (1u << (cause & 0x1F)))) target_priv = RV::PRIV_S;
    }

    if (target_priv == RV::PRIV_S) {
        s.scause = cause; s.sepc = s.pc; s.stval = tval;
        uint8_t sie = (s.mstatus & RV::MSTATUS_SIE) ? 1 : 0;
        s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_SPIE) | (sie ? RV::MSTATUS_SPIE : 0);
        s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_SIE);
        s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_SPP) | ((s.priv == RV::PRIV_S) ? RV::MSTATUS_SPP : 0);
        s.priv = RV::PRIV_S;
        // MODE=1 时中断走向量表（BASE + 4*cause），同步异常仍进 BASE
        s.pc = (is_irq && (s.stvec & 3) == 1) ? (s.stvec & ~(XLen)3) + 4u * (cause & 0x1F) : (s.stvec & ~(XLen)3);
    } else {
        s.mcause = cause; s.mepc = s.pc; s.mtval = tval;
        uint8_t mie = (s.mstatus & RV::MSTATUS_MIE) ? 1 : 0;
        s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_MPIE) | (mie ? RV::MSTATUS_MPIE : 0);
        // MPRV 在 trap 入口保持不变：规范里清 MPRV 只有两处——xRET 返回低于 M 的
        // 特权级时（见 MRET/SRET 的处理），以及系统复位。在 trap 入口清掉会让
        // "MPRV 下访存出错 → 进 M 态处理 → 返回重试"这条软件路径失去 MPP 语义，
        // 标准 riscv-tests 的 dirty.S 正是依赖 trap 后 MPRV 仍然有效
        s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_MIE);
        s.mstatus = (s.mstatus & ~(XLen)RV::MSTATUS_MPP) | ((uint32_t)s.priv << 11);
        s.priv = RV::PRIV_M;
        s.pc = (is_irq && (s.mtvec & 3) == 1) ? (s.mtvec & ~(XLen)3) + 4u * (cause & 0x1F) : (s.mtvec & ~(XLen)3);
    }
    return true;
}

// ============================================================
// Debug 触发模块（Sdtrig 的可选子集：mcontrol type=2）
// ============================================================
template<int XLEN> requires SupportedXlen<XLEN>
bool RiscVCPU<XLEN>::triggerMatch(const CpuState &s, XLen addr, uint32_t op_bit) {
    // tcontrol.mte 未置位时 M 态触发整体关闭；规范里它控制 M 态能否被触发
    if (!(s.tcontrol & 0x8u)) return false;
    if (s.priv != RV::PRIV_M) return false;
    for (int i = 0; i < RV::TRIGGER_NUM; i++) {
        const XLen t = s.tdata1[i];
        // type 在最高 4 位：RV32 是 31:28，RV64 是 63:60
        if ((uint32_t)(t >> (XLEN - 4)) != RV::TRIG_TYPE_MCONTROL) continue;
        if (!(t & RV::TRIG_M)) continue;      // 配置里没开 M 态匹配
        if (!(t & op_bit)) continue;          // 这次访存类型不在触发条件里
        if (s.tdata2[i] == addr) return true;
    }
    return false;
}

// ============================================================
// Sv32 / Sv39 MMU + TLB
// ============================================================
template<int XLEN> requires SupportedXlen<XLEN>
uint32_t RiscVCPU<XLEN>::checkPerms(uint32_t pte, bool is_write, bool is_exec, uint8_t priv, uint32_t mstatus) {
    uint32_t cause = is_write ? RV::EXC_SPF : is_exec ? RV::EXC_IPF : RV::EXC_LPF;
    if (is_write && !(pte & RV::PTE_W)) return cause;
    if (is_exec && !(pte & RV::PTE_X)) return cause;
    // 读权限：R=1，或 MXR=1 时执行页也可读
    if (!is_write && !is_exec && !(pte & RV::PTE_R) && !((mstatus & RV::MSTATUS_MXR) && (pte & RV::PTE_X))) return cause;
    if (priv == RV::PRIV_U && !(pte & RV::PTE_U)) return cause;
    // S 态取指 U 页直接 fault；load/store 访问 U 页要 mstatus.SUM
    if (priv == RV::PRIV_S && (pte & RV::PTE_U) && (is_exec || !(mstatus & RV::MSTATUS_SUM))) return cause;
    return 0;
}

template<int XLEN> requires SupportedXlen<XLEN>
bool RiscVCPU<XLEN>::handlePte(CpuState &s, uint64_t pte) {
    using P = SvParams<XLEN>;
    // 页错误按访存类型分档；访问错误用于"地址超出总线 32 位能力"这类硬件限制
    const uint32_t pf_cause = s.mmu_is_exec ? RV::EXC_IPF : (s.mmu_is_write ? RV::EXC_SPF : RV::EXC_LPF);
    const uint32_t af_cause = s.mmu_is_exec ? RV::EXC_IAF : (s.mmu_is_write ? RV::EXC_SAF : RV::EXC_LAF);
    auto fail = [&](uint32_t cause) {
        takeTrap(s, cause, (XLenOf<XLEN>)s.mmu_vaddr);
        s.phase = Phase::FETCH;
        return false;
    };

    // Sv39 的 PTE[63:54] 是保留位，非零的页表项属于保留编码，规范要求报 page fault
    if constexpr (XLEN == 64) {
        if (pte >> 54) return fail(pf_cause);
    }
    if (!(pte & RV::PTE_V)) return fail(pf_cause);
    // 保留编码只有 R=0,W=1（W 必须伴随 R）；R=0,W=0,X=1 是合法的 execute-only 页
    if (!(pte & RV::PTE_R) && (pte & RV::PTE_W)) return fail(pf_cause);

    const uint64_t ppn = (pte >> 10) & P::root_ppn_mask;
    const int level = s.mmu_level;

    if (pte & (RV::PTE_R | RV::PTE_W | RV::PTE_X)) {
        // 叶子。大页要求本级以下的 PPN 位为 0——那些位要接虚拟地址的对应 VPN，
        // 非零说明页表配错了，规范要求报 page fault 而不是丢掉这些位
        const uint64_t low_mask = (level == 0) ? 0 : ((1ull << (P::vpn_bits * level)) - 1);
        if (ppn & low_mask) return fail(pf_cause);
        if (uint32_t cause = checkPerms((uint32_t)pte, s.mmu_is_write, s.mmu_is_exec, s.mmu_priv, s.mstatus))
            return fail(cause);
        // A/D 位按规范有两种合法实现：硬件自动置位（Svadu）或触发 page fault 交给软件。
        // 标准 riscv-tests 的 dirty.S 两条路径都能过（它的 trap 处理程序会自己补 D 位），
        // 这里走 page fault 这条路，不在硬件里改写页表
        if (!(pte & RV::PTE_A) || (s.mmu_pte_is_write && !(pte & RV::PTE_D)))
            return fail(pf_cause);

        // 大页：把虚拟地址低 level 级的 VPN 接到 PPN 低位
        const uint64_t pa_ppn = (ppn & ~low_mask) | ((s.mmu_vaddr >> 12) & low_mask);
        // 总线只有 32 位地址线：物理地址超过 4GB 会被截断成别名，软件完全看不出
        if (pa_ppn > 0xFFFFFULL) return fail(af_cause);

        int idx = s.tlb_cycle % RV::TLB_SIZE; s.tlb_cycle++;
        auto &e = s.tlb[idx];
        e.vpn = (uint32_t)(s.mmu_vaddr >> 12);
        e.ppn = (uint32_t)pa_ppn;
        e.asid = (uint16_t)P::asid((uint64_t)s.satp);
        e.flags = (uint8_t)(pte & 0xFF);
        e.valid = true; e.global = (pte >> 5) & 1;
        // TLB 已装填：取指翻译回 FETCH，数据访问回 EXECUTE 重试原指令
        s.phase = s.mmu_is_exec ? Phase::FETCH : Phase::EXECUTE;
        return true;
    }

    // 非叶子 PTE：指向下一级页表。已经在叶级还是非叶子，说明页表少了一级
    if (level == 0) return fail(pf_cause);
    // 下一级页表的基址同样要落在 32 位地址线能表达的范围内
    if (ppn > 0xFFFFFULL) return fail(af_cause);
    const uint64_t next_base = ppn << 12;
    if (level == P::levels - 1) {
        // 最高一级的 PTE 缓存下来，同一区域的下一次遍历可以跳过这一级
        s.l1_cache_tag = s.mmu_vaddr >> (12 + P::vpn_bits * level);
        s.l1_cache_ppn = ppn;
        s.l1_cache_valid = true;
    }
    const int next_level = level - 1;
    s.mmu_level = (uint8_t)next_level;
    s.mmu_next_page = next_base;
    s.mmu_pte_addr = next_base +
        (((s.mmu_vaddr >> (12 + P::vpn_bits * next_level)) & P::vpn_field_mask) * P::pte_size);
    if (s.mmu_pte_addr > 0xFFFFFFFFULL) return fail(af_cause);
    // 本拍结束后才发起下一级的读，调用方负责拉低 stb
    s.phase = Phase::MMU_WALK_LO;
    return false;
}

template<int XLEN> requires SupportedXlen<XLEN>
bool RiscVCPU<XLEN>::mmuTranslate(CpuState &s, XLenOf<XLEN> vaddr, XLenOf<XLEN> &paddr,
                             bool is_write, bool is_exec) {
    // 地址线只有 32 位：高于 4GB 的物理地址会被截断后别名到低 4GB，
    // 软件完全看不出差异，所以在这里显式报访问错误。cause 按访存类型分档
    auto pass_through = [&]() -> bool {
        if constexpr (XLEN == 64) {
            if (((uint64_t)vaddr >> 32) != 0) {
                uint32_t cause = is_exec ? RV::EXC_IAF : (is_write ? RV::EXC_SAF : RV::EXC_LAF);
                takeTrap(s, cause, vaddr);
                return false;
            }
        }
        paddr = vaddr;
        return true;
    };
    // MPRV：M 态的数据访存按 mstatus.MPP 的特权级翻译（取指不受 MPRV 影响）
    uint8_t priv = s.priv;
    if (priv == RV::PRIV_M) {
        if (is_exec || !(s.mstatus & RV::MSTATUS_MPRV)) { return pass_through(); }
        uint8_t mpp = (uint8_t)((s.mstatus >> 11) & 3);
        if (mpp == RV::PRIV_M) { return pass_through(); }
        priv = mpp;
    }
    if (!pagingEnabled<XLEN>(s.satp)) [[unlikely]] { return pass_through(); }

    // Sv39 只定义了 39 位虚拟地址：高 25 位必须是 bit38 的符号扩展。
    // 非 canonical 地址按规范报 page fault——拿去翻页表的话高位会被静默丢弃，形成别名
    if constexpr (XLEN == 64) {
        const uint64_t hi = (uint64_t)vaddr >> 39;
        if (hi != 0 && hi != ((1ull << 25) - 1)) {
            takeTrap(s, is_exec ? RV::EXC_IPF : (is_write ? RV::EXC_SPF : RV::EXC_LPF), vaddr);
            return false;
        }
    }

    using P = SvParams<XLEN>;
    const uint32_t asid = P::asid((uint64_t)s.satp);
    const uint64_t vpn = (uint64_t)vaddr >> 12;
    // TLB 查找
    for (int i = 0; i < RV::TLB_SIZE; i++) {
        auto &e = s.tlb[i];
        if (e.valid && e.vpn == (uint32_t)vpn && (e.global || e.asid == asid)) [[likely]] {
            if (uint32_t cause = checkPerms(e.flags, is_write, is_exec, priv, s.mstatus))
            { takeTrap(s, cause, vaddr); return false; }
            // 软件会清 A/D 做页老化，所以命中时不能假定它已经置位：
            // 缺位就走慢路径重新遍历页表，由页表项里的 A/D 检查报 page fault。
            // 已置位的常规情况只多两次位测试。
            if (!(e.flags & RV::PTE_A) || (is_write && !(e.flags & RV::PTE_D)))
                break;
            {
                uint64_t pa64 = ((uint64_t) e.ppn << 12) | (uint64_t)(vaddr & 0xFFF);
                // 总线只有 32 位地址线：截断后会别名到低 4GB
                if (pa64 > 0xFFFFFFFFULL) {
                    uint32_t cause = is_exec ? RV::EXC_IAF : (is_write ? RV::EXC_SAF : RV::EXC_LAF);
                    takeTrap(s, cause, vaddr);
                    return false;
                }
                paddr = (XLen) pa64;
                return true;
            }
        }
    }

    // TLB miss → 通过 Wishbone 总线遍历页表
    s.mmu_vaddr = (uint64_t)vaddr;
    s.mmu_is_write = is_write;
    s.mmu_pte_is_write = is_write;   // 遍历期间不变，取指不会污染它
    s.mmu_is_exec = is_exec;
    s.mmu_priv = priv; // 遍历相位里做权限检查要用同一个特权级

    // 查最高级 PTE 缓存。缓存的是"最高级非叶子 PTE 的 PPN"，
    // tag 取最高级 VPN，命中时跳过这一级的总线读
    constexpr int top = P::levels - 1;
    const uint64_t tag = (uint64_t)vaddr >> (12 + P::vpn_bits * top);
    const bool hit = s.l1_cache_valid && s.l1_cache_tag == tag;
    const int level = hit ? (top - 1) : top;
    const uint64_t base = hit ? (s.l1_cache_ppn << 12)
                              : ((uint64_t)(s.satp & P::root_ppn_mask) << 12);
    // 页表本身也要落在 32 位地址线能表达的范围内，否则遍历读到的是别名的垃圾
    if (base > 0xFFFFFFFFULL) {
        takeTrap(s, is_exec ? RV::EXC_IAF : (is_write ? RV::EXC_SAF : RV::EXC_LAF), vaddr);
        return false;
    }
    s.mmu_level = (uint8_t)level;
    s.mmu_next_page = base;
    s.mmu_pte_addr = base +
        (((uint64_t)vaddr >> (12 + P::vpn_bits * level)) & P::vpn_field_mask) * P::pte_size;
    if (s.mmu_pte_addr > 0xFFFFFFFFULL) {
        takeTrap(s, is_exec ? RV::EXC_IAF : (is_write ? RV::EXC_SAF : RV::EXC_LAF), vaddr);
        return false;
    }
    s.phase = Phase::MMU_WALK_LO;
    return false; // 发起 MMU 遍历，原指令稍后重试
}

// 两份实现都编进这个 .cpp：RV32 是默认，RV64 供 OpenSBI/Linux 用。
// 放在这里而不是头文件，是为了让其它翻译单元只看到声明
template class RiscVCPU<32>;
template class RiscVCPU<64>;

} // namespace dsc
