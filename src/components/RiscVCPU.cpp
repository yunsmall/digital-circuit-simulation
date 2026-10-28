#include <cmath>
#include <limits>
#include <cstdio>
// RiscVCPU — RISC-V 软核 (RV32IMA + S/U/M + Sv32 MMU)
#include "dcs/components/RiscVCPU.h"
#include <cstring>
#include <format>

namespace dsc {

constexpr int SLOT = 16;
constexpr int I_CLK = 0, I_RST = 1, I_DAT = 2, I_ACK = 3, I_IRQ = 4, I_MTI = 5, I_MSI = 6, I_SEIP = 7;
constexpr int O_ADR = 0, O_WDAT = 1, O_WE = 2, O_SEL = 3, O_STB = 4, O_CYC = 5, O_HALT = 6, O_COUNT = 7;

// ============================================================
RiscVCPU::RiscVCPU(const std::string &name) : Component(name, "riscv_cpu") {
    addInput("clk", 1); addInput("rst", 1); addInput("wb_dat_i", 32);
    addInput("wb_ack_i", 1); addInput("irq", 1);
    addInput("mti", 1); addInput("msi", 1);
    addInput("seip", 1); // S 级外部中断，通常接 PLIC 的 S-mode context
    addOutput("wb_adr_o", 32, false, true); addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_we_o", 1, false, true); addOutput("wb_sel_o", 4, false, true);
    addOutput("wb_stb_o", 1, false, true); addOutput("wb_cyc_o", 1, false, true);
    addOutput("halted", 1, false, true);
    std::memset(&_state, 0, sizeof(_state));
    _state.priv = RV::PRIV_M;
}
RiscVCPU::~RiscVCPU() = default;
uint32_t RiscVCPU::reg(int idx) const { return (idx>=0&&idx<=31)?_state.regs[idx]:0; }
uint32_t RiscVCPU::pc() const { return _state.pc; }
bool RiscVCPU::halted() const { return _state.wfi_halted; }
bool RiscVCPU::prepare(std::string &) { _prepared = true; return true; }
std::vector<Component::JitSymbol> RiscVCPU::extraJitSymbols() const {
    return {{"_dcs_cpu_tick",reinterpret_cast<void*>(&cpuTick)},
            {"_dcs_cpu_reset",reinterpret_cast<void*>(&cpuReset)}};
}

std::string RiscVCPU::genStateDecl() const {
    return std::format("static void *_cpu_state_{0} = (void *)0x{1:X}ULL;\n",
                       _id, reinterpret_cast<uintptr_t>(&_state));
}
std::string RiscVCPU::genInitCode() const  { return std::format("    _dcs_cpu_reset(_cpu_state_{0});\n", _id); }
std::string RiscVCPU::genDeinitCode() const { return ""; }
std::string RiscVCPU::genResetCode() const { return std::format("    _dcs_cpu_reset(_cpu_state_{0});\n", _id); }

std::string RiscVCPU::genFuncDef_seq() const {
    int id = _id;
    std::string code;
    code += std::format("// RISC-V CPU: {}\n", name());
    code += "extern void _dcs_cpu_tick(void *, const uint8_t *const *, uint8_t *const *);\n";
    code += "extern void _dcs_cpu_reset(void *);\n\n";
    code += std::format("static void {}() {{\n", funcName_seq());
    code += genInputPtrArray("_in");
    // 输出与组合元件一样经 genOutputWrite：它同时写 _c_out 与 _w，
    // 避免本元件比别处多出一拍传播延迟
    code += std::format("    uint8_t _obuf[{}][{}];\n", O_COUNT, SLOT);
    code += "    dcs_memset(_obuf, 0, sizeof(_obuf));\n";
    code += std::format("    uint8_t *_op[{}] = {{", O_COUNT);
    for (int i = 0; i < O_COUNT; i++) code += std::format("{}_obuf[{}]", i ? "," : "", i);
    code += "};\n";
    code += std::format("    _dcs_cpu_tick(_cpu_state_{0}, _in, _op);\n", id);
    const int ow[O_COUNT] = {32, 32, 1, 4, 1, 1, 1};
    for (int i = 0; i < O_COUNT; i++)
        code += std::format("    {{ uint32_t _v=0; dcs_memcpy(&_v,_obuf[{}],4); {} }}\n", i,
                            genOutputWrite(i, "_v", ow[i]));
    code += "}\n";
    return code;
}
std::unique_ptr<Component> RiscVCPU::clone(const std::string &n) const {
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
void RiscVCPU::cpuTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs) {
    auto &s = *static_cast<CpuState*>(state_ptr);
    CpuIn in(inputs);
    if (in.rst) [[unlikely]] { cpuReset(state_ptr); packOut(outputs,0,0,false,0,false,false,false); return; }
    // 外部中断线要每 tick 反映到 mip，否则停机期间收不到中断
    if (in.irq) s.mip|=RV::IRQ_MEI; else s.mip&=~RV::IRQ_MEI;
    if (in.mti) s.mip|=RV::IRQ_MTI; else s.mip&=~RV::IRQ_MTI;
    if (in.msi) s.mip|=RV::IRQ_MSI; else s.mip&=~RV::IRQ_MSI;
    if (in.seip) s.mip|=RV::IRQ_SEI; else s.mip&=~RV::IRQ_SEI;
    // WFI 期间只要有中断挂起就唤醒：不看 mstatus.MIE，也不要求 mie 已使能
    // 与 WFI 的停机条件保持一致：只有本地使能的挂起中断才唤醒，
    // 否则会在"唤醒 - 发现无法处理 - 重新停机"之间反复抖动
    if (s.wfi_halted && (s.mip & s.mie)) {
        s.wfi_halted = false;
        s.phase = Phase::FETCH; // 唤醒后从暂停的 PC 继续取指
    }

    bool rising = !s.prev_clk && in.clk; s.prev_clk = in.clk;

    if (!rising) {
        uint32_t adr=0,wdat=0; bool we=false,stb=false,cyc=false; uint8_t sel=0;
        switch (s.phase) {
        // FETCH 这一拍地址要等上升沿翻译完才能定，不能先按 pc 发请求，
        // 否则从设备先把 pc 当地址做掉一次事务，页表读取会拿到取指数据
        case Phase::FETCH: adr=s.pc; sel=0x0F; stb=cyc=false; break;
        case Phase::WAIT_FETCH: adr=s.fetch_pa; sel=0x0F; stb=cyc=true; break;
        case Phase::WAIT_MEM: {
            // 与上升沿分支保持一致：拆分事务的拉低相位必须真的拉低 stb，
            // 否则同一时钟周期的另一半又把 stb 抬起来，从设备看不到事务边界。
            // stage 4 是"下一次上升沿才发起第二次事务"，这一拍必须继续拉低，
            // 否则会先用旧地址发一次假事务，从设备锁存旧地址后真正的事务再也不触发
            bool hold_off = (s.acc_stage >= 2 && s.acc_stage <= 4);
            adr=s.bus_adr; wdat=s.bus_wdat; we=s.bus_we; sel=s.bus_sel;
            stb=cyc=!hold_off;
            break;
        }
        case Phase::WAIT_MMU_L1: adr=s.mmu_l1_addr; sel=0x0F; stb=cyc=true; break;
        case Phase::WAIT_MMU_L2: adr=s.mmu_l2_addr; sel=0x0F; stb=cyc=true; break;
        case Phase::MMU_WALK_L1: adr=s.mmu_l1_addr; sel=0x0F; stb=cyc=true; break;
        case Phase::MMU_WALK_L2: adr=s.mmu_l2_addr; sel=0x0F; stb=cyc=true; break;
        case Phase::MMU_AD_WRITE: adr=s.mmu_pte_addr; wdat=s.mmu_pte; we=true; sel=0x0F; stb=cyc=true; break;
        // RMW 空拍：cyc 保持高表示总线仍被本核占用（别的设备插不进来），
        // 只拉低 stb 让从设备看到事务边界——AMO 的原子性就靠这个
        case Phase::AMO_GAP: adr=s.bus_adr; wdat=s.bus_wdat; sel=0x0F; stb=false; cyc=true; break;
        case Phase::AMO_WRITE: adr=s.bus_adr; wdat=s.bus_wdat; we=true; sel=0x0F; stb=cyc=true; break;
        case Phase::MMU_AD_GAP: adr=s.mmu_pte_addr; wdat=s.mmu_pte; sel=0x0F; stb=false; cyc=true; break;
        default: break;
        }
        packOut(outputs,adr,wdat,we,sel,stb,cyc,s.wfi_halted);
        return;
    }


    uint32_t wb_adr=0,wb_dat=0; bool wb_we=false,wb_stb=false,wb_cyc=false; uint8_t wb_sel=0;

    switch (s.phase) {
    case Phase::FETCH: {
        // 取指同样过 MMU：S/U 态开启 satp 后 pc 是虚拟地址
        uint32_t fpa=0;
        if (!mmuTranslate(s, s.pc, fpa, false, true)) [[unlikely]] {
            if (s.phase == Phase::MMU_WALK_L1 || s.phase == Phase::MMU_WALK_L2) {
                uint32_t adr = (s.phase == Phase::MMU_WALK_L1) ? s.mmu_l1_addr : s.mmu_l2_addr;
                wb_adr=adr; wb_sel=0x0F; wb_we=false; wb_stb=wb_cyc=true;
                s.phase = (s.phase == Phase::MMU_WALK_L1) ? Phase::WAIT_MMU_L1 : Phase::WAIT_MMU_L2;
                break;
            }
            // 页错误已进 trap，s.pc 现在是 trap 入口。M 态入口是物理地址可直接用；
            // S 态的 stvec 是虚拟地址，必须重新翻译，不能把它当物理地址取指。
            uint32_t fpa2=0;
            if (s.priv == RV::PRIV_M || (s.satp & RV::SATP_MODE) == 0) {
                s.fetch_pa = s.pc;
            } else if (!mmuTranslate(s, s.pc, fpa2, false, true)) {
                if (s.phase == Phase::MMU_WALK_L1 || s.phase == Phase::MMU_WALK_L2) {
                    uint32_t adr2 = (s.phase == Phase::MMU_WALK_L1) ? s.mmu_l1_addr : s.mmu_l2_addr;
                    wb_adr=adr2; wb_sel=0x0F; wb_we=false; wb_stb=wb_cyc=true;
                    s.phase = (s.phase == Phase::MMU_WALK_L1) ? Phase::WAIT_MMU_L1 : Phase::WAIT_MMU_L2;
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
        {
            uint32_t ic_idx = (fpa >> 2) & 255u;
            uint32_t ic_tag = fpa >> 10;
            if (s.icache[ic_idx].valid && s.icache[ic_idx].tag == ic_tag) {
                s.ic_hits++;
                s.inst = s.icache[ic_idx].inst;
                s.phase = Phase::EXECUTE; // 命中就不再发起总线读
                wb_stb = wb_cyc = false;
                break;
            }
            s.ic_misses++;
        }
        wb_adr=fpa; wb_sel=0x0F; wb_stb=wb_cyc=true; s.phase=Phase::WAIT_FETCH;
        break;
    }

    case Phase::WAIT_FETCH:
        if (in.ack) {
            s.inst=in.dat;
            uint32_t ic_idx = (s.fetch_pa >> 2) & 255u;
            s.icache[ic_idx].tag = s.fetch_pa >> 10;
            s.icache[ic_idx].inst = in.dat;
            s.icache[ic_idx].valid = true;
            wb_stb=wb_cyc=false; s.phase=Phase::EXECUTE;
        }
        else { wb_adr=s.fetch_pa; wb_sel=0x0F; wb_stb=wb_cyc=true; }
        break;

    case Phase::EXECUTE: {
        uint32_t trap_cause=0;
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
        else if (s.phase == Phase::MMU_WALK_L1 || s.phase == Phase::MMU_WALK_L2) {
            // mmuTranslate 设置了 MMU 遍历阶段，输出第一次总线读请求并立即转为等待状态
            uint32_t mmu_adr = (s.phase == Phase::MMU_WALK_L1) ? s.mmu_l1_addr : s.mmu_l2_addr;
            wb_adr = mmu_adr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
            s.phase = (s.phase == Phase::MMU_WALK_L1) ? Phase::WAIT_MMU_L1 : Phase::WAIT_MMU_L2;
        }
        else if (need_mem) [[unlikely]] { s.bus_adr=wb_adr; s.bus_wdat=wb_dat; s.bus_we=wb_we; s.bus_sel=wb_sel; s.phase=Phase::WAIT_MEM; }
        else { s.phase=Phase::FETCH; wb_sel=0x0F; wb_stb=wb_cyc=false; }
        break;
    }

    case Phase::WAIT_MEM:
        // 两次事务之间的相位：主从都是时序元件，_w 传递有一拍延迟，
        // 只拉低一拍 stb 不足以让从设备忘记上一次 ack，必须占满这些拍再重新发起。
        if (s.acc_stage >= 2 && s.acc_stage <= 5) [[unlikely]] {
            if (s.acc_stage <= 3) { // 拉低 stb，等从设备看到事务结束
                wb_adr = s.bus_adr; wb_dat = s.bus_wdat; wb_we = s.bus_we; wb_sel = s.bus_sel;
                wb_stb = wb_cyc = false;
                s.acc_stage++;
                break;
            }
            if (s.acc_stage == 4) { // 发起第二次事务
                uint32_t p = s.acc_va & 3;
                uint32_t a2 = (s.acc_va & ~3u) + 4;
                uint32_t dat = 0;
                uint8_t sel = 0x0F;
                if (s.bus_we) {
                    uint32_t n = (s.acc_f3==RV::F3_SB)?1:(s.acc_f3==RV::F3_SH)?2:4;
                    uint32_t first = 4 - p;
                    uint32_t rest = n - first;
                    dat = s.acc_raw >> (8 * first);
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
                    // AMO: 计算写回值，发起写请求
                    uint32_t ld_val=raw, new_val=0, f5=s.bus_amo_f5;
                    uint32_t rs2_val=s.bus_wdat;
                    switch (f5) {
                    case RV::F5_LR:      s.lr_addr=s.bus_adr; s.lr_valid=true; s.regs[s.bus_rd]=ld_val; // LR: 像普通 LW 一样返回
                                         wb_dat=0; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=false; s.bus_amo=false; s.phase=Phase::FETCH; break;
                    case RV::F5_SC:      if(s.lr_valid && s.lr_addr==s.bus_adr) { s.regs[s.bus_rd]=0; new_val=rs2_val; s.lr_valid=false; }
                                         else { s.regs[s.bus_rd]=1; s.lr_valid=false; s.bus_amo=false; wb_dat=0; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=false; s.phase=Phase::FETCH; break; }
                                         goto amo_write;
                    // AMO 算术: 先保存旧值到 rd，然后 goto amo_write 写回新值
                    case RV::F5_AMOSWAP: new_val=rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOADD:  new_val=ld_val+rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOXOR:  new_val=ld_val^rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOAND:  new_val=ld_val&rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOOR:   new_val=ld_val|rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOMIN:  new_val=((int32_t)ld_val<(int32_t)rs2_val)?ld_val:rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOMAX:  new_val=((int32_t)ld_val>(int32_t)rs2_val)?ld_val:rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOMINU: new_val=(ld_val<rs2_val)?ld_val:rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    case RV::F5_AMOMAXU: new_val=(ld_val>rs2_val)?ld_val:rs2_val; s.regs[s.bus_rd]=ld_val; goto amo_write;
                    default: s.bus_amo=false; wb_dat=0; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=false; s.phase=Phase::FETCH; break;
                    }
                    break;
                // SC 的 reg[rd] 已在 case 中设置(0或1)，跳到此时不覆盖
                amo_write:
                    if (s.lr_valid && s.lr_addr == (s.bus_adr & ~3u)) s.lr_valid = false; // AMO 写也算覆盖保留字
                    s.bus_wdat=new_val; s.bus_we=true;
                    // 读事务到此结束，先空一拍拉低 stb，下一拍再发独立的写事务
                    s.phase = Phase::AMO_GAP;
                    break;
                } else {
                    // 普通读；跨字非对齐访问要再读一个字，与第一次读回的内容拼接
                    if (s.acc_stage == 1) {
                        s.acc_raw = raw;
                        s.acc_stage = 2;      // 进入两次事务之间的拉低阶段
                        wb_adr = s.bus_adr; wb_dat=0; wb_we=false; wb_sel=0x0F;
                        wb_stb = wb_cyc = false;
                        break;
                    }
                    uint32_t value;
                    if (s.acc_stage >= 6) {
                        uint32_t sh = 8 * (s.acc_va & 3); // 跨字时 p 必不为 0，移位量 8/16/24 都安全
                        value = (s.acc_raw >> sh) | (raw << (32 - sh));
                        s.acc_stage = 0;
                    } else {
                        value = raw >> (8 * (s.acc_va & 3));
                    }
                    uint32_t result=0;
                    switch (s.bus_funct3) {
                    case RV::F3_LB: { int8_t b=(int8_t)(value&0xFF); result=(uint32_t)(int32_t)b; break; }
                    case RV::F3_LH: { int16_t h=(int16_t)(value&0xFFFF); result=(uint32_t)(int32_t)h; break; }
                    case RV::F3_LW: result=value; break;
                    case RV::F3_LBU: result=value&0xFF; break;
                    case RV::F3_LHU: result=value&0xFFFF; break;
                    }
                    if (s.bus_fp) s.fregs[s.bus_rd]=value; // FLW：结果进 f 寄存器
                    else s.regs[s.bus_rd]=result;
                    wb_dat=0; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=false; s.phase=Phase::FETCH;
                }
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
                s.acc_stage = 0;
                wb_dat=0; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=false; s.phase=Phase::FETCH;
            }
        }
        break;

    case Phase::MMU_WALK_L1:
        // 发起 L1 PTE 读请求
        wb_adr = s.mmu_l1_addr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        s.phase = Phase::WAIT_MMU_L1;
        break;

    case Phase::WAIT_MMU_L1:
        wb_adr = s.mmu_l1_addr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        if (in.ack) {
            uint32_t l1e = in.dat;
            // 检查 V 位
            if (!(l1e & 1)) {
                takeTrap(s, s.mmu_is_write ? RV::EXC_SPF : s.mmu_is_exec ? RV::EXC_IPF : RV::EXC_LPF, s.mmu_vaddr);
                s.phase = Phase::FETCH; wb_sel = 0x0F; wb_stb = wb_cyc = false;
                break;
            }
            // 保留编码只有 R=0,W=1（W 必须伴随 R）；R=0,W=0,X=1 是合法的 execute-only 页
            if (!(l1e & 2) && (l1e & 4)) {
                takeTrap(s, s.mmu_is_write ? RV::EXC_SPF : s.mmu_is_exec ? RV::EXC_IPF : RV::EXC_LPF, s.mmu_vaddr);
                s.phase = Phase::FETCH; wb_sel = 0x0F; wb_stb = wb_cyc = false;
                break;
            }
            // 缓存 L1 PTE（用于后续 TLB miss 时的快速路径）
            s.l1_cache_tag = s.mmu_vaddr >> 22;
            s.l1_cache_ppn = l1e >> 10;
            s.l1_cache_valid = true;
            if ((l1e & 0xE) != 0) {
                // 超页面（4MB）：L1 是叶子，没有 L2，不能让这个 PPN 进入 L2 缓存
                s.l1_cache_valid = false;
                // 权限检查
                if (uint32_t cause = checkPerms(l1e, s.mmu_is_write, s.mmu_is_exec, s.mmu_priv, s.mstatus)) {
                    takeTrap(s, cause, s.mmu_vaddr);
                    s.phase = Phase::FETCH; wb_sel = 0x0F; wb_stb = wb_cyc = false;
                    break;
                }
                // 大页的 A/D 同样要落到内存页表上，否则与 4KB 页路径行为不一致
                {
                    uint32_t upd = l1e;
                    if (!(l1e & RV::PTE_A)) upd |= RV::PTE_A;
                    if (s.mmu_is_write && !(l1e & RV::PTE_D)) upd |= RV::PTE_D;
                    if (upd != l1e) {
                        s.mmu_pte = upd; s.mmu_pte_addr = s.mmu_l1_addr; s.mmu_pte_is_l1 = true;
                        s.phase = Phase::MMU_AD_GAP;
                        wb_adr = s.mmu_pte_addr; wb_dat = upd; wb_we = true; wb_sel = 0x0F;
                        wb_stb = wb_cyc = true;
                        break;
                    }
                }
                int idx = s.tlb_cycle % RV::TLB_SIZE; s.tlb_cycle++;
                auto &e = s.tlb[idx];
                e.vpn = s.mmu_vaddr >> 12; e.ppn = ((l1e >> 20) << 10) | ((s.mmu_vaddr >> 12) & 0x3FF);
                e.asid = (uint16_t)((s.satp >> 22) & 0x1FF); e.flags = (uint8_t)(l1e & 0xFF); // satp[30:22] 是 ASID
                e.valid = true; e.global = (l1e >> 5) & 1;
                s.phase = s.mmu_is_exec ? Phase::FETCH : Phase::EXECUTE;
                wb_stb = wb_cyc = false;
            } else {
                // 非叶子 PTE（R=W=X=0）：指向 L2 页表
                s.mmu_l2_addr = ((l1e >> 10) << 12) | ((s.mmu_vaddr >> 12) & 0x3FF) * 4;
                s.phase = Phase::MMU_WALK_L2;
                wb_stb = wb_cyc = false; // 本拍拉低，下拍 MMU_WALK_L2 重新断言 stb 上升沿
            }
        }
        break;

    case Phase::MMU_WALK_L2:
        // 发起 L2 PTE 读请求
        wb_adr = s.mmu_l2_addr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        s.phase = Phase::WAIT_MMU_L2;
        break;

    case Phase::WAIT_MMU_L2:
        wb_adr = s.mmu_l2_addr; wb_sel = 0x0F; wb_we = false; wb_stb = wb_cyc = true;
        if (in.ack) {
            uint32_t l2e = in.dat;
            bool fault = !(l2e & 1); // V=0
            if (!fault) fault = (!(l2e & 2) && (l2e & 4)); // 保留组合: R=0,W=1 非法
            if (!fault) {
                if (uint32_t cause = checkPerms(l2e, s.mmu_is_write, s.mmu_is_exec, s.mmu_priv, s.mstatus))
                { takeTrap(s, cause, s.mmu_vaddr);
                  s.phase = Phase::FETCH; wb_sel = 0x0F; wb_stb = wb_cyc = false; break; }
                // A/D 位必须落到内存页表上，否则软件做页老化会看到过期值
                uint32_t upd = l2e;
                if (!(l2e & RV::PTE_A)) upd |= RV::PTE_A;
                if (s.mmu_is_write && !(l2e & RV::PTE_D)) upd |= RV::PTE_D;
                if (upd != l2e) {
                    // 注意：这里沿用同一笔事务把 we 从 0 翻到 1（stb 不拉低），
                    // 与 AMO 的写回路径一致，依赖从设备按 we 上升沿识别写操作
                    s.mmu_pte = upd; s.mmu_pte_addr = s.mmu_l2_addr; s.mmu_pte_is_l1 = false;
                    s.phase = Phase::MMU_AD_GAP;
                    wb_adr = s.mmu_pte_addr; wb_dat = upd; wb_we = true; wb_sel = 0x0F;
                    wb_stb = wb_cyc = true;
                    break;
                }
                // 装填 TLB
                int idx = s.tlb_cycle % RV::TLB_SIZE; s.tlb_cycle++;
                auto &e = s.tlb[idx];
                e.vpn = s.mmu_vaddr >> 12; e.ppn = (l2e >> 10) & 0x3FFFFF;
                e.asid = (uint16_t)((s.satp >> 22) & 0x1FF); e.flags = (uint8_t)(l2e & 0xFF);
                e.valid = true; e.global = (l2e >> 5) & 1;
                // TLB 已装填：取指翻译回 FETCH，数据访问回 EXECUTE 重试原指令
                s.phase = s.mmu_is_exec ? Phase::FETCH : Phase::EXECUTE;
                wb_stb = wb_cyc = false;
            } else {
                takeTrap(s, s.mmu_is_write ? RV::EXC_SPF : s.mmu_is_exec ? RV::EXC_IPF : RV::EXC_LPF, s.mmu_vaddr);
                s.phase = Phase::FETCH; wb_sel = 0x0F; wb_stb = wb_cyc = false;
            }
        }
        break;

    case Phase::AMO_GAP:
        s.phase = Phase::AMO_WRITE;
        break;
    case Phase::AMO_WRITE:
        wb_adr = s.bus_adr; wb_dat = s.bus_wdat; wb_we = true; wb_sel = 0x0F; wb_stb = wb_cyc = true;
        if (in.ack) {
            // 与普通写完成后的收尾保持一致：清 AMO 标志、复位访问阶段、结束事务
            s.bus_amo = false;
            s.acc_stage = 0;
            wb_stb = wb_cyc = false;
            s.phase = Phase::FETCH;
        }
        break;
    case Phase::MMU_AD_GAP:
        s.phase = Phase::MMU_AD_WRITE;
        break;
    case Phase::MMU_AD_WRITE:
        wb_adr = s.mmu_pte_addr; wb_dat = s.mmu_pte; wb_we = true; wb_sel = 0x0F; wb_stb = wb_cyc = true;
        if (in.ack) {
            // 页表已更新，按更新后的 PTE 装填 TLB
            int idx = s.tlb_cycle % RV::TLB_SIZE; s.tlb_cycle++;
            auto &e = s.tlb[idx];
            e.vpn = s.mmu_vaddr >> 12;
            e.ppn = s.mmu_pte_is_l1
                        ? (((s.mmu_pte >> 20) << 10) | ((s.mmu_vaddr >> 12) & 0x3FF))
                        : ((s.mmu_pte >> 10) & 0x3FFFFF);
            e.asid = (uint16_t)((s.satp >> 22) & 0x1FF); e.flags = (uint8_t)(s.mmu_pte & 0xFF);
            e.valid = true; e.global = (s.mmu_pte >> 5) & 1;
            s.phase = s.mmu_is_exec ? Phase::FETCH : Phase::EXECUTE;
            wb_stb = wb_cyc = false;
        }
        break;

    case Phase::HALTED: break;
    }
    s.regs[0]=0;
    packOut(outputs, wb_adr, wb_dat, wb_we, wb_sel, wb_stb, wb_cyc, s.wfi_halted);
}

void RiscVCPU::cpuReset(void *state_ptr) {
    auto &s = *static_cast<CpuState*>(state_ptr);
    std::memset(&s,0,sizeof(s));
    s.phase=Phase::FETCH; s.priv=RV::PRIV_M;
    for (auto &l : s.icache) l.valid = false;
}

// ============================================================
// execute
// ============================================================
static bool csrImplemented(uint32_t addr); // 定义在 csrRead 之前，这里先声明

void RiscVCPU::execute(CpuState &s, uint32_t inst,
                       uint32_t &wb_adr, uint32_t &wb_dat, bool &wb_we,
                       uint8_t &wb_sel, bool &wb_stb, bool &wb_cyc, bool &need_mem) {
    s.regs[0]=0; need_mem=false;
    uint32_t op=(inst>>RV::SH_OP)&RV::M_OP, rd=(inst>>RV::SH_RD)&RV::M_RD;
    uint32_t f3=(inst>>RV::SH_F3)&RV::M_F3, rs1=(inst>>RV::SH_RS1)&RV::M_RS1;
    uint32_t rs2=(inst>>RV::SH_RS2)&RV::M_RS2, f7=(inst>>RV::SH_F7)&RV::M_F7;
    int32_t ii=(int32_t)inst>>20, is=((int32_t)((inst>>7)&0x1F)|((int32_t)((inst>>25)&0x7F)<<5));
    is=(is<<20)>>20;
    int32_t ib=((int32_t)((inst>>8)&0xF)<<1)|((int32_t)((inst>>25)&0x3F)<<5)|
               ((int32_t)((inst>>7)&1)<<11)|((int32_t)((inst>>31)&1)<<12); ib=(ib<<19)>>19;
    int32_t iu=(int32_t)(inst&0xFFFFF000);
    int32_t ij=((int32_t)((inst>>21)&0x3FF)<<1)|((int32_t)((inst>>20)&1)<<11)|
               ((int32_t)((inst>>12)&0xFF)<<12)|((int32_t)((inst>>31)&1)<<20); ij=(ij<<11)>>11;
    uint32_t v1=s.regs[rs1], v2=s.regs[rs2];
    int32_t s1=(int32_t)v1, s2=(int32_t)v2;
    uint32_t npc=s.pc+4;

    switch (op) {
    case RV::OP_IMM: switch(f3){
        case RV::F3_ADDI:s.regs[rd]=v1+(uint32_t)ii; break;
        case RV::F3_SLLI:s.regs[rd]=v1<<(rs2&0x1F); break;
        case RV::F3_SLTI:s.regs[rd]=(s1<ii)?1:0; break;
        case RV::F3_SLTIU:s.regs[rd]=(v1<(uint32_t)ii)?1:0; break;
        case RV::F3_XORI:s.regs[rd]=v1^(uint32_t)ii; break;
        case RV::F3_SRLI:s.regs[rd]=(f7==RV::F7_SUB_SRA)?(uint32_t)(s1>>(rs2&0x1F)):v1>>(rs2&0x1F); break;
        case RV::F3_ORI:s.regs[rd]=v1|(uint32_t)ii; break;
        case RV::F3_ANDI:s.regs[rd]=v1&(uint32_t)ii; break;
        } break;
    case RV::LUI: s.regs[rd]=(uint32_t)iu; break;
    case RV::AUIPC: s.regs[rd]=s.pc+(uint32_t)iu; break;
    case RV::OP: {
    // M 扩展：funct7=0x01 时 funct3 选择乘除法，与普通运算共用同一 opcode
    if (f7 == 0x01) {
        switch (f3) {
        case 0: s.regs[rd] = (uint32_t)((uint64_t)v1 * v2); break;                          // MUL 低 32 位
        case 1: s.regs[rd] = (uint32_t)((uint64_t)((int64_t)s1 * (int64_t)s2) >> 32); break; // MULH 有符号×有符号高 32 位
        case 2: s.regs[rd] = (uint32_t)((uint64_t)((int64_t)s1 * (int64_t)(uint64_t)v2) >> 32); break; // MULHSU
        case 3: s.regs[rd] = (uint32_t)(((uint64_t)v1 * (uint64_t)v2) >> 32); break;         // MULHU
        // 除零和 INT_MIN/-1 的商与余数由规范直接规定，硬件不能靠 C 的未定义行为
        case 4: s.regs[rd] = (v2 == 0) ? 0xFFFFFFFFu : (v1 == 0x80000000u && v2 == 0xFFFFFFFFu) ? 0x80000000u : (uint32_t)(s1 / s2); break;
        case 5: s.regs[rd] = (v2 == 0) ? 0xFFFFFFFFu : (v1 / v2); break;
        case 6: s.regs[rd] = (v2 == 0) ? v1 : (v1 == 0x80000000u && v2 == 0xFFFFFFFFu) ? 0u : (uint32_t)(s1 % s2); break;
        case 7: s.regs[rd] = (v2 == 0) ? v1 : (v1 % v2); break;
        }
        break;
    }
    switch(f3){
        case RV::F3_ADD:s.regs[rd]=(f7==RV::F7_SUB_SRA)?v1-v2:v1+v2; break;
        case RV::F3_SLL:s.regs[rd]=v1<<(v2&0x1F); break;
        case RV::F3_SLT:s.regs[rd]=(s1<s2)?1:0; break;
        case RV::F3_SLTU:s.regs[rd]=(v1<v2)?1:0; break;
        case RV::F3_XOR:s.regs[rd]=v1^v2; break;
        case RV::F3_SRL:s.regs[rd]=(f7==RV::F7_SUB_SRA)?(uint32_t)(s1>>(v2&0x1F)):v1>>(v2&0x1F); break;
        case RV::F3_OR:s.regs[rd]=v1|v2; break;
        case RV::F3_AND:s.regs[rd]=v1&v2; break;
        } break;
    }
    case RV::JAL: {
        uint32_t tgt=s.pc+(uint32_t)ij;
        if (tgt & 2u) [[unlikely]] { takeTrap(s, RV::EXC_IAM, tgt); return; } // IALIGN=32
        s.regs[rd]=s.pc+4; npc=tgt; break;
    }
    case RV::JALR: {
        if(f3!=0) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        uint32_t tgt=(v1+(uint32_t)ii)&~1u;
        if (tgt & 2u) [[unlikely]] { takeTrap(s, RV::EXC_IAM, tgt); return; }
        s.regs[rd]=s.pc+4; npc=tgt; break;
    }
    case RV::BRANCH: { if (f3 == 2 || f3 == 3) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        bool t=false; switch(f3){
        case RV::F3_BEQ:t=(v1==v2);break; case RV::F3_BNE:t=(v1!=v2);break;
        case RV::F3_BLT:t=(s1<s2);break; case RV::F3_BGE:t=(s1>=s2);break;
        case RV::F3_BLTU:t=(v1<v2);break; case RV::F3_BGEU:t=(v1>=v2);break;
        } if(t) { uint32_t tgt=s.pc+(uint32_t)ib;
            if (tgt & 2u) [[unlikely]] { takeTrap(s, RV::EXC_IAM, tgt); return; }
            npc=tgt; } } break;

    // ---- LOAD ----
    case RV::LOAD: {
        if (f3 == 3 || f3 > 5) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; } // 保留的 load 宽度编码
        uint32_t va=v1+(uint32_t)ii, pa;
        if (!mmuTranslate(s, va, pa, false, false)) [[unlikely]] return; // 缺页已 trap
        // 总线只接受字对齐地址，字内偏移由 CPU 自己记着；跨字则在 WAIT_MEM 里再读一个字
        uint32_t p=pa&3;
        uint32_t n=(f3==RV::F3_LH||f3==RV::F3_LHU)?2:(f3==RV::F3_LB||f3==RV::F3_LBU)?1:4;
        // MPRV 下 M 态的数据访问同样走分页，此时跨页检查不能因为 priv==M 就跳过，
        // 否则非对齐拆分会对第二个字直接用物理地址，跳过了另外一页的翻译
        bool _paging_active = (s.satp & RV::SATP_MODE) &&
                              (s.priv != RV::PRIV_M ||
                               ((s.mstatus & RV::MSTATUS_MPRV) &&
                                (((s.mstatus >> 11) & 3u) != RV::PRIV_M)));
        if (_paging_active && ((pa & 0xFFF) + n > 0x1000)) {
            // 跨页时第二个字不属于同一个物理页，需要第二次翻译（当前状态机不支持），
            // 按规范允许的做法触发 misaligned load。必须在置 need_mem 之前返回，
            // 否则 EXECUTE 之后仍会按残留的 need_mem 发起一次错误访存。
            s.acc_stage = 0;
            takeTrap(s, RV::EXC_LAM, va);
            return;
        }
        wb_adr=pa&~3u; wb_we=false; wb_sel=0x0F;
        wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa&~3u; s.bus_funct3=(uint8_t)f3; s.bus_rd=(uint8_t)rd; s.bus_sel=0x0F; s.bus_fp=0;
        s.acc_va=pa; s.acc_f3=(uint8_t)f3; s.acc_stage=(uint8_t)((p+n>4)?1:0);
        } break; // 不 return，让 PC 更新 + need_mem 走 WAIT_MEM

    // ---- STORE ----
    case RV::STORE: {
        if (f3 > 2) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; } // 保留的 store 宽度编码
        uint32_t va=v1+(uint32_t)is, pa;
        if (!mmuTranslate(s, va, pa, true, false)) [[unlikely]] return;
        // 总线只接受字对齐地址：先写字内剩余部分，跨字的高段留到 WAIT_MEM 补写
        uint32_t p=pa&3;
        uint32_t n=(f3==RV::F3_SB)?1:(f3==RV::F3_SH)?2:4;
        uint32_t first=(p+n>4)?(4-p):n;
        // MPRV 下 M 态的数据访问同样走分页，此时跨页检查不能因为 priv==M 就跳过，
        // 否则非对齐拆分会对第二个字直接用物理地址，跳过了另外一页的翻译
        bool _paging_active = (s.satp & RV::SATP_MODE) &&
                              (s.priv != RV::PRIV_M ||
                               ((s.mstatus & RV::MSTATUS_MPRV) &&
                                (((s.mstatus >> 11) & 3u) != RV::PRIV_M)));
        if (_paging_active && ((pa & 0xFFF) + n > 0x1000)) {
            s.acc_stage = 0;
            takeTrap(s, RV::EXC_SAM, va); // 跨页非对齐写同上，只能报错
            return;
        }
        wb_adr=pa&~3u; wb_we=true;
        wb_sel=(uint8_t)(((1u<<first)-1u)<<p); wb_dat=v2<<(8*p);
        s.acc_va=pa; s.acc_raw=v2; s.acc_f3=(uint8_t)f3; s.acc_stage=(uint8_t)((n>first)?1:0);
        if (s.lr_valid) { // 同 hart 存储覆盖保留地址所在的字即失效
            uint32_t w0 = pa & ~3u;
            if (s.lr_addr == w0 || (s.acc_stage == 1 && s.lr_addr == w0 + 4)) s.lr_valid = false;
        }
        wb_stb=wb_cyc=true; need_mem=true; s.bus_adr=pa&~3u; s.bus_wdat=wb_dat; s.bus_we=true; s.bus_sel=wb_sel; s.bus_funct3=0; s.bus_rd=0;
        } break; // 不 return，让 PC 更新 + need_mem 走 WAIT_MEM

    // ---- AMO ----
    case RV::AMO: {
        s.acc_stage = 0; // AMO 是单次对齐事务，不能继承上一次非对齐访存的拆分级数
        if (f3 != 2) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; } // 仅支持 .W
        switch ((inst >> 27) & 0x1F) { // 未实现的 AMO 编码必须报非法指令，不能静默完成
        case RV::F5_LR: case RV::F5_SC: case RV::F5_AMOSWAP: case RV::F5_AMOADD:
        case RV::F5_AMOXOR: case RV::F5_AMOAND: case RV::F5_AMOOR:
        case RV::F5_AMOMIN: case RV::F5_AMOMAX: case RV::F5_AMOMINU: case RV::F5_AMOMAXU:
            break;
        default: [[unlikely]] takeTrap(s, RV::EXC_II, inst); return;
        }
        if (v1 & 3) [[unlikely]] { takeTrap(s, RV::EXC_LAM, v1); return; } // AMO 必须自然对齐
        uint32_t va=v1, pa;
        // LR 只读，只要求页有 R；SC 与其余 AMO 要写，要求 W
        bool amo_write = (((inst >> 27) & 0x1F) != RV::F5_LR);
        if (!mmuTranslate(s, va, pa, amo_write, false)) [[unlikely]] return;
        // AMO: 先发起读请求，WAIT_MEM 中完成修改后自动回写
        wb_adr=pa&~3u; wb_dat=v2; wb_we=false; wb_sel=0x0F;
        wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa; s.bus_funct3=f3; s.bus_rd=rd;
        s.bus_sel=0x0F; s.bus_we=false; s.bus_amo=true;
        s.bus_amo_f5=(uint8_t)((inst>>27)&0x1F); // funct5
        s.bus_wdat=v2; // rs2 值用于 AMO 计算
        } break; // 不 return，让 PC 更新 + need_mem 走 WAIT_MEM

    // ---- 浮点访存（FLW / FSW，自然对齐；非对齐按规范触发异常）----
    case RV::LOAD_FP: {
        if (f3 != 2) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        uint32_t va=v1+(uint32_t)ii, pa;
        if (!mmuTranslate(s, va, pa, false, false)) [[unlikely]] return;
        if (pa & 3u) [[unlikely]] { takeTrap(s, RV::EXC_LAM, va); return; }
        wb_adr=pa; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa; s.bus_funct3=2; s.bus_rd=(uint8_t)rd; s.bus_fp=1; s.acc_stage=0;
        } break;

    case RV::STORE_FP: {
        if (f3 != 2) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; }
        uint32_t va=v1+(uint32_t)is, pa;
        if (!mmuTranslate(s, va, pa, true, false)) [[unlikely]] return;
        if (pa & 3u) [[unlikely]] { takeTrap(s, RV::EXC_SAM, va); return; }
        if (s.lr_valid && s.lr_addr == (pa & ~3u)) s.lr_valid = false;
        wb_adr=pa; wb_we=true; wb_sel=0x0F; wb_dat=s.fregs[rs2]; wb_stb=wb_cyc=true; need_mem=true;
        s.bus_adr=pa; s.bus_wdat=wb_dat; s.bus_we=true; s.bus_sel=0x0F; s.bus_fp=0; s.acc_stage=0;
        } break;

    // ---- 融合乘加：FMADD / FMSUB / FNMSUB / FNMADD ----
    case 0x43: case 0x47: case 0x4B: case 0x4F: {
        auto u2f = [](uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; };
        auto f2u = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        if (((inst >> 25) & 3) != 0) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); return; } // 只实现单精度
        uint32_t rs3 = (inst >> 27) & 0x1F;
        float a = u2f(s.fregs[rs1]), b = u2f(s.fregs[rs2]), c = u2f(s.fregs[rs3]);
        // 规范要求一次舍入，所以用 double 保存乘积再组合，最后才转 float
        double prod = (double)a * (double)b;
        double exact = (op == 0x43) ? prod + (double)c
                     : (op == 0x47) ? prod - (double)c
                     : (op == 0x4B) ? -prod + (double)c
                                    : -prod - (double)c;
        float r = (float)exact;
        s.fregs[rd] = std::isnan(r) ? 0x7FC00000u : f2u(r); // 同样规范化 NaN
        // sNaN 参与任何浮点运算都要报 NV，不只是"输入全不是 NaN"的非法运算
        bool fma_snan = ((s.fregs[rs1] >> 23) & 0xFF) == 0xFF && (s.fregs[rs1] & 0x7FFFFFu) && !(s.fregs[rs1] & 0x400000u)
                        || ((s.fregs[rs2] >> 23) & 0xFF) == 0xFF && (s.fregs[rs2] & 0x7FFFFFu) && !(s.fregs[rs2] & 0x400000u)
                        || ((s.fregs[rs3] >> 23) & 0xFF) == 0xFF && (s.fregs[rs3] & 0x7FFFFFu) && !(s.fregs[rs3] & 0x400000u);
        if (std::isnan(r)) {
            if (fma_snan || (!std::isnan(a) && !std::isnan(b) && !std::isnan(c))) s.fcsr |= 0x10;
        } else if (std::isfinite(exact)) {
            if ((double)r != exact) s.fcsr |= 0x01;          // NX
            if (std::isinf(r)) s.fcsr |= 0x04;                // OF：有限结果舍入成 inf
            else if (r == 0.0f && exact != 0.0) s.fcsr |= 0x02; // UF
        }
        s.mstatus |= (3u << 13);
        break;
    }

    // ---- 单精度浮点运算（F 扩展），寄存器内容是 IEEE754 位模式 ----
    case RV::OP_FP: {
        auto u2f = [](uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; };
        auto f2u = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        float a = u2f(s.fregs[rs1]), b = u2f(s.fregs[rs2]);
        // qNaN 是静默的，只有 signaling NaN 才触发无效运算
        auto is_snan = [](uint32_t u) {
            return ((u >> 23) & 0xFF) == 0xFF && (u & 0x7FFFFFu) != 0 && !(u & 0x400000u);
        };
        bool snan_in = is_snan(s.fregs[rs1]) || is_snan(s.fregs[rs2]);
        uint32_t rm = f3;
        auto setf = [&](float v) { s.fregs[rd] = f2u(v); };
        // exact 用 double 保存未舍入的结果，据此判断是否不精确（NX）
        auto setf_flags = [&](double exact) {
            float r = (float) exact;
            if (std::isnan(r)) {
                // 规范要求产生 canonical NaN，不同平台算出的 NaN 位模式不统一
                s.fregs[rd] = 0x7FC00000u;
                // sNaN 输入本身就是非法运算；其余情况是"非 NaN 输入算出了 NaN"
                if (snan_in || (!std::isnan(a) && !std::isnan(b))) s.fcsr |= 0x10;
                return;
            }
            setf(r);
            if (!std::isfinite(exact)) return;
            if ((double) r != exact) s.fcsr |= 0x01; // NX
            if (std::isinf(r)) s.fcsr |= 0x04;       // OF
            else if (r == 0.0f && exact != 0.0) s.fcsr |= 0x02; // UF
        };

        switch (f7) {
        case RV::F7_FADD: setf_flags((double)a + (double)b); break;
        case RV::F7_FSUB: setf_flags((double)a - (double)b); break;
        case RV::F7_FMUL: setf_flags((double)a * (double)b); break;
        case RV::F7_FDIV:
            // DZ 只适用于"有限非零被除数 / 零"；inf/0 的结果是 inf，不是错误
            if (b == 0.0f && std::isfinite(a) && a != 0.0f) s.fcsr |= 0x08;
            setf_flags((double)a / (double)b);
            break;
        case RV::F7_FSQRT:
            if (a < 0.0f) { s.fcsr |= 0x10; setf(std::numeric_limits<float>::quiet_NaN()); }
            else setf_flags(std::sqrt((double)a));
            break;
        case RV::F7_FSGNJ: {
            uint32_t ua = s.fregs[rs1], ub = s.fregs[rs2], sign;
            switch (f3) {
            case 0: sign = ub & 0x80000000u; break;          // FSGNJ
            case 1: sign = (~ub) & 0x80000000u; break;       // FSGNJN
            case 2: sign = (ua ^ ub) & 0x80000000u; break;   // FSGNJX
            default: takeTrap(s, RV::EXC_II, inst); return;  // f3 >= 3 是保留编码
            }
            s.fregs[rd] = (ua & 0x7FFFFFFFu) | sign;
            break;
        }
        case RV::F7_FMINMAX:
            if (f3 >= 2) { takeTrap(s, RV::EXC_II, inst); return; } // f3 >= 2 是保留编码
            // NaN 参与时返回另一个操作数并置 NV；两个都是 NaN 才返回 canonical NaN
            if (snan_in) s.fcsr |= 0x10;
            if (std::isnan(a) && std::isnan(b)) s.fregs[rd] = 0x7FC00000u;
            else if (std::isnan(a)) setf(b);
            else if (std::isnan(b)) setf(a);
            else if (f3 == 0) {
                if (a == 0.0f && b == 0.0f) s.fregs[rd] = s.fregs[rs1] | s.fregs[rs2]; // -0 优先
                else setf(a < b ? a : b);
            } else {
                if (a == 0.0f && b == 0.0f) s.fregs[rd] = s.fregs[rs1] & s.fregs[rs2]; // +0 优先
                else setf(a > b ? a : b);
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
            float r;
            switch (rm) {
            case 1: r = std::trunc(a); break;   // RTZ
            case 2: r = std::floor(a); break;   // RDN
            case 3: r = std::ceil(a); break;    // RUP
            case 4: r = std::round(a); break;   // RMM
            default: r = std::nearbyint(a); break; // RNE
            }
            if (std::isfinite(a) && std::isfinite(r) && r != a) s.fcsr |= 0x01; // 截掉小数部分即不精确
            if (rs2 == 0) { // FCVT.W.S
                if (std::isnan(r) || r >= 2147483648.0f) { s.regs[rd] = 0x7FFFFFFFu; s.fcsr |= 0x10; }
                else if (r < -2147483648.0f) { s.regs[rd] = 0x80000000u; s.fcsr |= 0x10; }
                else s.regs[rd] = (uint32_t)(int32_t)r;
            } else { // FCVT.WU.S
                if (std::isnan(r) || r >= 4294967296.0f) { s.regs[rd] = 0xFFFFFFFFu; s.fcsr |= 0x10; }
                else if (r <= -1.0f) { s.regs[rd] = 0u; s.fcsr |= 0x10; }
                else s.regs[rd] = (uint32_t)r;
            }
            break;
        }
        case RV::F7_FCVT_S_W: {
            if (rs2 >= 2) { takeTrap(s, RV::EXC_II, inst); return; } // rs2 >= 2 是保留编码
            // uint32 → float 会丢精度（如 0xFFFFFFFF），规范要求此时设 NX
            double d = rs2 == 0 ? (double) (int32_t) v1 : (double) v1;
            float f = (float) d;
            if ((double) f != d) s.fcsr |= 0x01;
            setf(f);
            break;
        }
        case RV::F7_FMV_X_W:
            if (f3 == 0) { // FMV.X.W
                s.regs[rd] = s.fregs[rs1];
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
            s.fregs[rd] = v1;
            break;
        default:
            takeTrap(s, RV::EXC_II, inst); return;
        }
        s.mstatus |= (3u << 13); // FS = Dirty
        break;
    }

    case RV::MISC_MEM:
        // FENCE.I 是软件告诉硬件"我改过指令内存了"，必须让指令缓存失效
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
                s.mstatus = (s.mstatus & ~RV::MSTATUS_MIE) | ((s.mstatus & RV::MSTATUS_MPIE)>>4);
                s.mstatus |= RV::MSTATUS_MPIE; s.mstatus &= ~RV::MSTATUS_MPP;
                npc = s.mepc;
                break;
            }
            case RV::F12_SRET: {
                if (s.priv == RV::PRIV_U) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
                s.priv = (s.mstatus & RV::MSTATUS_SPP) ? RV::PRIV_S : RV::PRIV_U;
                s.mstatus = (s.mstatus & ~RV::MSTATUS_SIE) | ((s.mstatus & RV::MSTATUS_SPIE)>>4);
                s.mstatus |= RV::MSTATUS_SPIE; s.mstatus &= ~RV::MSTATUS_SPP;
                npc = s.sepc;
                break;
            }
            case RV::F12_WFI:
                // 有中断挂起就继续执行；否则停在这里，PC 不前进
                if (!(s.mip & s.mie)) {
                    s.wfi_halted = true;
                    s.phase = Phase::HALTED; // 停止取指，等中断唤醒
                    npc = s.pc;
                }
                break;
            default:
                // SFENCE.VMA: funct7=0x09, 刷新全部 TLB
                if (f7 == 0x09) {
                    for (auto &e : s.tlb) e.valid = false;
                    s.l1_cache_valid = false; // 页表可能被改过，L1 缓存同样失效
                } else {
                    takeTrap(s, RV::EXC_II, inst); npc = s.pc;
                }
                break;
            }
        } else {
            if (f3 == 4) [[unlikely]] { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; } // funct3=4 未定义
            uint32_t csr_addr=(inst>>20)&RV::M_F12, csr_val=csrRead(s, csr_addr);
            // 访问未实现的 CSR、或写只读 CSR（csr[11:10]==3 且确实是写类指令）都是非法指令
            bool csr_write = (f3 != RV::F3_CSRRS && f3 != RV::F3_CSRRC) || rs1 != 0;
            if (!csrImplemented(csr_addr) || (((csr_addr >> 10) & 3) == 3 && csr_write)) [[unlikely]]
            { takeTrap(s, RV::EXC_II, inst); npc = s.pc; break; }
            uint32_t wval = (f3>=RV::F3_CSRRW&&f3<=RV::F3_CSRRC) ? v1 : (uint32_t)rs1;
            // 检查 CSR 访问权限 (最低两位 = 读写权限位)
            if ((csr_addr>>8)&3) { uint8_t min_priv=(uint8_t)((csr_addr>>8)&3); if(s.priv<min_priv) [[unlikely]] { takeTrap(s,RV::EXC_II,inst); npc=s.pc; break; }}
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
    if (!need_mem) { wb_adr=s.pc; wb_we=false; wb_sel=0x0F; wb_stb=wb_cyc=true; }
}

// ============================================================
// CSR 读写
// ============================================================
// 已实现的 CSR 白名单：不在这里的地址一律当非法指令处理，避免软件误以为读写生效
static bool csrImplemented(uint32_t addr) {
    // PMP 配置/地址寄存器：本核不实现保护，但寄存器必须存在，软件会写 0 来关闭它
    if (addr >= 0x3A0 && addr <= 0x3AF) return true;
    if (addr >= 0x3B0 && addr <= 0x3EF) return true;
    switch (addr) {
    case RV::CSR_FFLAGS: case RV::CSR_FRM: case RV::CSR_FCSR:
    case 0x00F:  // vcsr
    case 0x306:  // mcounteren
    // mstatush 在 RV32 上只有 SBE/MBE/GVA/MPV 等位。本核是大端不支持、也没有
    // 客户机虚拟化，这些位全部硬连 0：读回 0，写入忽略，即下面的默认分支行为
    case 0x310:  // mstatush
    case 0xC00: case 0xC01: case 0xC02: // cycle / time / instret
    case 0xC80: case 0xC81: case 0xC82: // cycleh / timeh / instreth
    case 0xF11: case 0xF12: case 0xF13: case 0xF14: // vendorid / archid / impid / hartid
        return true;
    case RV::CSR_SSTATUS: case RV::CSR_SIE: case RV::CSR_STVEC: case RV::CSR_SSCRATCH:
    case RV::CSR_SEPC: case RV::CSR_SCAUSE: case RV::CSR_STVAL: case RV::CSR_SIP: case RV::CSR_SATP:
    case RV::CSR_MSTATUS: case RV::CSR_MISA: case RV::CSR_MEDELEG: case RV::CSR_MIDELEG:
    case RV::CSR_MIE: case RV::CSR_MTVEC: case RV::CSR_MSCRATCH: case RV::CSR_MEPC:
    case RV::CSR_MCAUSE: case RV::CSR_MTVAL: case RV::CSR_MIP:
        return true;
    default:
        return false;
    }
}

uint32_t RiscVCPU::csrRead(const CpuState &s, uint32_t addr) {
    switch (addr) {
    case RV::CSR_FFLAGS:  return s.fcsr & 0x1Fu;
    case RV::CSR_FRM:     return (s.fcsr >> 5) & 0x7u;
    case RV::CSR_FCSR:    return s.fcsr & 0xFFu;
    // SD 是只读汇总位：FS 或 XS 非 0 时必须为 1，软件靠它判断是否需要保存浮点上下文
    case RV::CSR_MSTATUS: return s.mstatus | ((((s.mstatus >> 13) & 3u) != 0) ? (1u << 31) : 0u);
    // MXL=1(RV32) | I(8) | M(12) | A(0) | F(5) | S(18) | U(20) —— 与实现的指令集和特权级一一对应
    case RV::CSR_MISA:    return 0x40141100u | (1u<<0) | (1u<<5);
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
    case RV::CSR_SSTATUS: return (s.mstatus & 0x000DE122u) | ((((s.mstatus >> 13) & 3u) != 0) ? (1u << 31) : 0u);
    case RV::CSR_SIE:     return s.mie & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI);
    case RV::CSR_STVEC:   return s.stvec;
    case RV::CSR_SSCRATCH:return s.sscratch;
    case RV::CSR_SEPC:    return s.sepc;
    case RV::CSR_SCAUSE:  return s.scause;
    case RV::CSR_STVAL:   return s.stval;
    case RV::CSR_SIP:     return s.mip & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI);
    case RV::CSR_SATP:    return s.satp;
    default: return 0;
    }
}
void RiscVCPU::csrWrite(CpuState &s, uint32_t addr, uint32_t val) {
    switch (addr) {
    case RV::CSR_FFLAGS:  s.fcsr = (s.fcsr & ~0x1Fu) | (val & 0x1Fu); break;
    case RV::CSR_FRM:     s.fcsr = (s.fcsr & ~0xE0u) | ((val & 0x7u) << 5); break;
    case RV::CSR_FCSR:    s.fcsr = val & 0xFFu; break;
    // 本核没有额外扩展状态、也不支持非字节序：
    //   XS(15:16) 恒 0、UBE(6) 恒 0、bit23 是 WPRI 保留位，都不能写
    case RV::CSR_MSTATUS: s.mstatus = val & 0x007E79AAu; break;  // 掩码含 SIE/MIE/SPIE/MPIE/SPP/MPP/FS/MPRV/SUM/MXR/TVM/TW/TSR；UBE 与 XS 本核恒 0，故不在掩码内
    case RV::CSR_MEDELEG: s.medeleg = val & 0xFFFF; break;
    case RV::CSR_MIDELEG: s.mideleg = val & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI); break;
    case RV::CSR_MIE:     s.mie = val & (RV::IRQ_SSI|RV::IRQ_MSI|RV::IRQ_STI|RV::IRQ_MTI|RV::IRQ_SEI|RV::IRQ_MEI); break;
    case RV::CSR_MTVEC:   s.mtvec = val & ~2u; break; // 保留 bit0（MODE），只清保留位 bit1
    case RV::CSR_MSCRATCH:s.mscratch = val; break;
    case RV::CSR_MEPC:    s.mepc = val & ~3u; break; // IALIGN=32，低两位恒为 0
    case RV::CSR_MCAUSE:  s.mcause = val; break;
    case RV::CSR_MTVAL:   s.mtval = val; break;
    case RV::CSR_SSTATUS: s.mstatus = (s.mstatus & ~0x000C6122u) | (val & 0x000C6122u); break; // 去掉 XS[1:0]，SD(bit31) 只读
    case RV::CSR_SIE:     s.mie = (s.mie & ~(RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)) | (val & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)); break;
    case RV::CSR_MIP:     s.mip = (s.mip & ~(RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)) | (val & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)); break;
    case RV::CSR_SIP:     s.mip = (s.mip & ~(RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)) | (val & (RV::IRQ_SSI|RV::IRQ_STI|RV::IRQ_SEI)); break;
    case RV::CSR_STVEC:   s.stvec = val & ~2u; break;
    case RV::CSR_SSCRATCH:s.sscratch = val; break;
    case RV::CSR_SEPC:    s.sepc = val & ~3u; break;
    case RV::CSR_SCAUSE:  s.scause = val; break;
    case RV::CSR_STVAL:   s.stval = val; break;
    case RV::CSR_SATP: {
        uint32_t mode = val >> 31;
        if (mode == 0 || mode == 1) {
            s.satp = val;
            for (auto &e : s.tlb) e.valid = false;
            s.l1_cache_valid = false; // 换页表后旧的 L1 缓存指向旧根页表
        }
        break;
    }
    default: break;
    }
}

// ============================================================
// 中断裁决（考虑委托）
// ============================================================
bool RiscVCPU::takeInterrupt(CpuState &s, uint32_t &trap_cause) {
    uint32_t pending = s.mie & s.mip;
    if (!pending) [[likely]] return false; // 绝大多数时候无中断等待

    // M 级中断不可委托；S 级中断只有 mideleg 置位后才归 S 处理
    uint32_t deleg = s.mideleg & (RV::IRQ_SSI | RV::IRQ_STI | RV::IRQ_SEI);
    uint32_t pend_s = pending & deleg;
    uint32_t pend_m = pending & ~deleg;

    // 全局使能位只约束本级：当前特权级低于目标级时，中断总是使能
    bool m_on = (s.priv < RV::PRIV_M) || (s.mstatus & RV::MSTATUS_MIE);
    // M 态不会被更低优先级的 S 中断打断
    bool s_on = (s.priv != RV::PRIV_M) && ((s.priv < RV::PRIV_S) || (s.mstatus & RV::MSTATUS_SIE));

    // 优先级: MEI > MSI > MTI > SEI > SSI > STI
    if (m_on && pend_m) {
        if (pend_m & RV::IRQ_MEI) { trap_cause = RV::MCAUSE_IRQ | 11; return true; }
        if (pend_m & RV::IRQ_MSI) { trap_cause = RV::MCAUSE_IRQ | 3; return true; }
        if (pend_m & RV::IRQ_MTI) { trap_cause = RV::MCAUSE_IRQ | 7; return true; }
        if (pend_m & RV::IRQ_SEI) { trap_cause = RV::MCAUSE_IRQ | 9; return true; }
        if (pend_m & RV::IRQ_SSI) { trap_cause = RV::MCAUSE_IRQ | 1; return true; }
        if (pend_m & RV::IRQ_STI) { trap_cause = RV::MCAUSE_IRQ | 5; return true; }
    }
    if (s_on && pend_s) {
        if (pend_s & RV::IRQ_SEI) { trap_cause = RV::MCAUSE_IRQ | 9; return true; }
        if (pend_s & RV::IRQ_SSI) { trap_cause = RV::MCAUSE_IRQ | 1; return true; }
        if (pend_s & RV::IRQ_STI) { trap_cause = RV::MCAUSE_IRQ | 5; return true; }
    }
    return false;
}

// ============================================================
// 同步异常陷阱入口
// ============================================================
bool RiscVCPU::takeTrap(CpuState &s, uint32_t cause, uint32_t tval) {
    bool is_irq = (cause >> 31) != 0;
    uint8_t target_priv = RV::PRIV_M;

    // 检查委托
    if (s.priv <= RV::PRIV_S) {
        if (is_irq && (s.mideleg & (1u << (cause & 0x1F)))) target_priv = RV::PRIV_S;
        else if (!is_irq && (s.medeleg & (1u << (cause & 0x1F)))) target_priv = RV::PRIV_S;
    }

    if (target_priv == RV::PRIV_S) {
        s.scause = cause; s.sepc = s.pc; s.stval = tval;
        uint8_t sie = (s.mstatus & RV::MSTATUS_SIE) ? 1 : 0;
        s.mstatus = (s.mstatus & ~RV::MSTATUS_SPIE) | (sie ? RV::MSTATUS_SPIE : 0);
        s.mstatus = (s.mstatus & ~RV::MSTATUS_SIE);
        s.mstatus = (s.mstatus & ~RV::MSTATUS_SPP) | ((s.priv == RV::PRIV_S) ? RV::MSTATUS_SPP : 0);
        s.priv = RV::PRIV_S;
        // MODE=1 时中断走向量表（BASE + 4*cause），同步异常仍进 BASE
        s.pc = (is_irq && (s.stvec & 3) == 1) ? (s.stvec & ~3u) + 4u * (cause & 0x1F) : (s.stvec & ~3u);
    } else {
        s.mcause = cause; s.mepc = s.pc; s.mtval = tval;
        uint8_t mie = (s.mstatus & RV::MSTATUS_MIE) ? 1 : 0;
        s.mstatus = (s.mstatus & ~RV::MSTATUS_MPIE) | (mie ? RV::MSTATUS_MPIE : 0);
        s.mstatus = (s.mstatus & ~RV::MSTATUS_MIE);
        s.mstatus = (s.mstatus & ~RV::MSTATUS_MPP) | ((uint32_t)s.priv << 11);
        s.priv = RV::PRIV_M;
        s.pc = (is_irq && (s.mtvec & 3) == 1) ? (s.mtvec & ~3u) + 4u * (cause & 0x1F) : (s.mtvec & ~3u);
    }
    return true;
}

// ============================================================
// Sv32 MMU + TLB
// ============================================================
uint32_t RiscVCPU::checkPerms(uint32_t pte, bool is_write, bool is_exec, uint8_t priv, uint32_t mstatus) {
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

bool RiscVCPU::mmuTranslate(CpuState &s, uint32_t vaddr, uint32_t &paddr,
                             bool is_write, bool is_exec) {
    // MPRV：M 态的数据访存按 mstatus.MPP 的特权级翻译（取指不受 MPRV 影响）
    uint8_t priv = s.priv;
    if (priv == RV::PRIV_M) {
        if (is_exec || !(s.mstatus & RV::MSTATUS_MPRV)) { paddr = vaddr; return true; }
        uint8_t mpp = (uint8_t)((s.mstatus >> 11) & 3);
        if (mpp == RV::PRIV_M) { paddr = vaddr; return true; }
        priv = mpp;
    }
    if ((s.satp & RV::SATP_MODE) == 0) [[unlikely]] { paddr = vaddr; return true; }

    uint32_t vpn = vaddr >> 12;
    // TLB 查找
    for (int i = 0; i < RV::TLB_SIZE; i++) {
        auto &e = s.tlb[i];
        if (e.valid && e.vpn == vpn && (e.global || e.asid == ((s.satp >> 22) & 0x1FF))) [[likely]] {
            if (uint32_t cause = checkPerms(e.flags, is_write, is_exec, priv, s.mstatus))
            { takeTrap(s, cause, vaddr); return false; }
            // 软件会清 A/D 做页老化，所以命中时不能假定它已经置位：
            // 缺位就走慢路径重新遍历页表，由 MMU_AD_WRITE 把 A/D 写回内存。
            // 已置位的常规情况只多两次位测试。
            if (!(e.flags & 0x40) || (is_write && !(e.flags & 0x80)))
                break;
            paddr = (e.ppn << 12) | (vaddr & 0xFFF);
            return true;
        }
    }

    // TLB miss → 通过 Wishbone 总线遍历页表
    s.mmu_vaddr = vaddr;
    s.mmu_is_write = is_write;
    s.mmu_is_exec = is_exec;
    s.mmu_priv = priv; // 遍历相位里做权限检查要用同一个特权级
    s.mmu_root = (s.satp & 0x3FFFFF) << 12;

    // 查 L1 PTE 缓存（命中→跳过 L1 总线读）
    uint32_t l1_tag = vaddr >> 22;
    if (s.l1_cache_valid && s.l1_cache_tag == l1_tag) [[likely]] {
        s.mmu_l2_addr = (s.l1_cache_ppn << 12) | ((vaddr >> 12) & 0x3FF) * 4;
        s.phase = Phase::MMU_WALK_L2;
    } else {
        s.mmu_l1_addr = s.mmu_root + (l1_tag << 2);
        s.phase = Phase::MMU_WALK_L1;
    }
    return false; // 发起 MMU 遍历，原指令稍后重试
}

} // namespace dsc
