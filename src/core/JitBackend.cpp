// JitBackend — 把生成的 C 源码编译成可调用代码，并解析入口函数指针
//
// 这里只做"编译与符号解析"，不碰网表、不生成代码、不参与 tick。
// tick 走的是编译后解析出的函数指针，所以本类的方法只在 Circuit::compile() 里调用一次，
// 不会给每 tick 的路径增加任何间接层。
#include "dcs/JitBackend.h"

#include <cstring>
#include <format>

#ifdef DCS_USE_TCC
#include <libtcc.h>
#else
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/PreprocessorOptions.h>
#if defined(__linux__)
#include <llvm/ExecutionEngine/Orc/Debugging/PerfSupportPlugin.h>
#endif
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/MC/TargetRegistry.h>
#include <cstdlib>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#endif

namespace dsc {

namespace {
#ifdef DCS_USE_TCC
// TCCState 已由 libtcc.h 在全局作用域声明；这里再加一个会让 dsc 内的名字遮蔽它
#else
// IR 优化集合，DCS_JIT_PASS_SET=none|basic|inline 可覆盖。
// 默认 basic：只做 mem2reg/instcombine/gvn/sccp/dce 这类纯精简 pass，不会让生成的
// 代码变大；inline 模式会跑 Inliner（代码膨胀、icache 压力上升），实测没有稳定收益，
// 所以默认不开。none 用于排查生成代码本身的问题
void runJitPasses(llvm::Module &m) {
    const char *set = std::getenv("DCS_JIT_PASS_SET");
    const std::string_view mode = set ? std::string_view(set) : std::string_view("basic");
    if (mode == "none")
        return;
    const bool with_inline = mode == "inline";

    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::PromotePass()); // mem2reg
    fpm.addPass(llvm::EarlyCSEPass());
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::SimplifyCFGPass());
    fpm.addPass(llvm::GVNPass());
    fpm.addPass(llvm::SCCPPass());
    fpm.addPass(llvm::DCEPass());

    llvm::ModulePassManager mpm;
    if (with_inline)
        mpm.addPass(pb.buildInlinerPipeline(llvm::OptimizationLevel::O2,
                                            llvm::ThinOrFullLTOPhase::None));
    mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    mpm.run(m, mam);
}

void initCompilerEnv() {
    // 测试夹具会并行编译多个电路，一次性初始化必须线程安全
    static std::once_flag once;
    std::call_once(once, [] {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmParser();
        LLVMInitializeX86AsmPrinter();
    });
}
#endif
} // namespace

struct JitBackend::Impl {
#ifdef DCS_USE_TCC
    TCCState *tcc_state = nullptr;
#else
    std::unique_ptr<llvm::LLVMContext> llvm_ctx;
    std::unique_ptr<llvm::orc::LLJIT> jit;
#endif
};

JitBackend::JitBackend() : _impl(new Impl()) {}

JitBackend::~JitBackend() {
    cleanup();
    delete _impl;
}

void JitBackend::cleanup() {
#ifdef DCS_USE_TCC
    if (_impl->tcc_state) {
        tcc_delete(_impl->tcc_state);
        _impl->tcc_state = nullptr;
    }
#else
    _impl->jit.reset();
    _impl->llvm_ctx.reset();
#endif
}

JitCompileResult JitBackend::compile(const std::string &c_code,
                                     const std::string &shim_path,
                                     void *circuit_ptr,
                                     const std::vector<std::pair<std::string, void *>> &symbols) {
    cleanup();

#ifdef DCS_USE_TCC
    auto *s = tcc_new();
    if (!s)
        return {ErrorCode::JIT_COMPILE_FAILED, -1, "tcc_new 失败"};

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_set_options(s, "-nostdlib -O2");
    if (!shim_path.empty())
        tcc_add_sysinclude_path(s, shim_path.c_str());

    tcc_add_symbol(s, "dcs_bus_conflict",
                   reinterpret_cast<void *>(&dcsBusConflictThunk));
    // -nostdlib 下 TCC 处理大块初始化/结构体赋值时会自行发出 memset/memcpy 调用
    {
        using memset_fn = void *(*)(void *, int, unsigned long long);
        using memcpy_fn = void *(*)(void *, const void *, unsigned long long);
        using memmove_fn = void *(*)(void *, const void *, unsigned long long);
        static memset_fn s_memset = [](void *d, int c, unsigned long long n) { return std::memset(d, c, (size_t) n); };
        static memcpy_fn s_memcpy = [](void *d, const void *s2, unsigned long long n) { return std::memcpy(d, s2, (size_t) n); };
        static memmove_fn s_memmove = [](void *d, const void *s2, unsigned long long n) { return std::memmove(d, s2, (size_t) n); };
        tcc_add_symbol(s, "memset", reinterpret_cast<void *>(s_memset));
        tcc_add_symbol(s, "memcpy", reinterpret_cast<void *>(s_memcpy));
        tcc_add_symbol(s, "memmove", reinterpret_cast<void *>(s_memmove));
        // 生成代码里统一叫 dcs_*：TCC 不做优化，交给 libc 的向量化实现更快；
        // LLVM 那边同名符号是 __builtin 的 inline 版本
        tcc_add_symbol(s, "dcs_memcpy", reinterpret_cast<void *>(s_memcpy));
        tcc_add_symbol(s, "dcs_memset", reinterpret_cast<void *>(s_memset));
    }
    for (auto &[name, ptr]: symbols)
        tcc_add_symbol(s, name.c_str(), ptr);

    if (tcc_compile_string(s, c_code.c_str()) == -1) {
        tcc_delete(s);
        return {ErrorCode::JIT_COMPILE_FAILED, -1, "TCC 编译失败"};
    }
    // 重定位失败（-1）仍把 s 存进 tcc_state 的话，后续符号查找会全军覆没，
    // 报出来的却是 JIT_SYMBOL_FAILED，把真正的链接问题掩盖掉
    if (tcc_relocate(s) < 0) {
        tcc_delete(s);
        return {ErrorCode::JIT_LINK_FAILED, -1, "TCC 重定位失败"};
    }
    _impl->tcc_state = s;

    JitCompileResult r;
    auto sym = [s](const char *n) { return reinterpret_cast<void *>(tcc_get_symbol(s, n)); };
    r.tick = reinterpret_cast<void (*)()>(sym("circuit_tick"));
    r.tick_cycle = reinterpret_cast<void (*)(int)>(sym("circuit_tick_cycle"));
    r.tick_cycles = reinterpret_cast<void (*)(int, uint64_t)>(sym("circuit_tick_cycles"));
    r.tick_batch = reinterpret_cast<void (*)(uint64_t)>(sym("circuit_tick_batch"));
    r.tick_forever = reinterpret_cast<void (*)()>(sym("circuit_tick_forever"));
    r.init = reinterpret_cast<void (*)()>(sym("circuit_init"));
    r.deinit = reinterpret_cast<void (*)()>(sym("circuit_deinit"));
    r.reset = reinterpret_cast<void (*)()>(sym("circuit_reset"));
    r.set_wire = reinterpret_cast<void (*)(int, const uint8_t *)>(sym("circuit_set_wire"));
    r.set_wire_u64 = reinterpret_cast<void (*)(int, uint64_t)>(sym("circuit_set_wire_u64"));
    r.get_wire = reinterpret_cast<void (*)(int, uint8_t *)>(sym("circuit_get_wire"));
    if (!r.tick || !r.init || !r.set_wire || !r.get_wire) {
        cleanup();
        return {ErrorCode::JIT_SYMBOL_FAILED, -1, "查找 JIT 符号失败"};
    }
    return r;

#else
    initCompilerEnv();

    _impl->llvm_ctx = std::make_unique<llvm::LLVMContext>();

    auto mem_buf = llvm::MemoryBuffer::getMemBuffer(c_code, "circuit.c");
    auto ci = std::make_unique<clang::CompilerInstance>();
    ci->createDiagnostics();
    auto &invoc = ci->getInvocation();
    invoc.getTargetOpts().Triple = llvm::sys::getDefaultTargetTriple();
    invoc.getTargetOpts().CPU = "x86-64"; // 基线 x86-64，避免 CI VM 中 SIGILL
    invoc.getLangOpts().C99 = true;
    invoc.getLangOpts().C11 = true;
    invoc.getLangOpts().CPlusPlus = false;
    invoc.getLangOpts().Bool = true;
    invoc.getLangOpts().Freestanding = true; // 不依赖标准库
    // 只开后端 codegen 优化，不在 IR 上加 PassBuilder 流水线。实测过 O2/O3 的
    // 完整优化流水线：always_inline 虽然会生效，但 circuit_tick 被内联成一个
    // 巨型函数，icache 压力抵消了收益，吞吐反而下降 5~15%，JIT 编译时间翻倍。
    // 生成的代码本来就是展开好的直线代码，不需要编译器再做大量改写
    invoc.getCodeGenOpts().OptimizationLevel = 2;
    invoc.getCodeGenOpts().RelocationModel = llvm::Reloc::PIC_;

    if (!shim_path.empty()) {
        auto &hdr = invoc.getHeaderSearchOpts();
        hdr.AddPath(shim_path, clang::frontend::System, false, false);
    }
    invoc.getPreprocessorOpts().UsePredefines = true;
    invoc.getFrontendOpts().Inputs.push_back(clang::FrontendInputFile("circuit.c", clang::Language::C));
    invoc.getFrontendOpts().ProgramAction = clang::frontend::EmitLLVMOnly;
    ci->getPreprocessorOpts().addRemappedFile("circuit.c", mem_buf.release());

    auto action = std::make_unique<clang::EmitLLVMOnlyAction>(_impl->llvm_ctx.get());
    if (!ci->ExecuteAction(*action)) {
        cleanup();
        return {ErrorCode::JIT_COMPILE_FAILED, -1, "Clang 编译 C 代码失败"};
    }
    auto module = action->takeModule();
    runJitPasses(*module);

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        cleanup();
        return {ErrorCode::JIT_LINK_FAILED, -1, "检测目标机失败: " + llvm::toString(jtmb.takeError())};
    }
    jtmb->setCPU("x86-64");
    auto jit_or_err = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit_or_err) {
        auto llvm_err = "创建 LLJIT 失败: " + llvm::toString(jit_or_err.takeError());
        cleanup();
        return {ErrorCode::JIT_LINK_FAILED, -1, llvm_err};
    }
    _impl->jit = std::move(*jit_or_err);
#if defined(__linux__)
    // JIT 出来的代码没有符号表，perf 只能看到一串地址。这个插件把每次链接的
    // 代码范围写进 jitdump，perf record -k 1 之后用 perf inject --jit 就能还原函数名。
    // Windows 上没有 perf，编译期直接跳过。
    if (auto perf_plugin = llvm::orc::PerfSupportPlugin::Create(
                _impl->jit->getExecutionSession().getExecutorProcessControl(),
                _impl->jit->getMainJITDylib(), false, false)) {
        static_cast<llvm::orc::ObjectLinkingLayer &>(_impl->jit->getObjLinkingLayer())
                .addPlugin(std::move(*perf_plugin));
    }
#endif

    // 统一入口：注入总线冲突回调与各元件暴露的符号
    {
        llvm::orc::SymbolMap syms;
        auto add = [&](const std::string &name, void *ptr) {
            syms[_impl->jit->mangleAndIntern(name)] = {
                    llvm::orc::ExecutorAddr::fromPtr(ptr),
                    llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
        };
        add("dcs_bus_conflict", reinterpret_cast<void *>(&dcsBusConflictThunk));
        for (auto &[name, ptr]: symbols)
            add(name, ptr);
        if (auto err = _impl->jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(syms)))) {
            auto sym_err = "注入 JIT 符号失败: " + llvm::toString(std::move(err));
            cleanup();
            return {ErrorCode::JIT_SYMBOL_FAILED, -1, sym_err};
        }
    }

    // module 里的类型都在 _impl->llvm_ctx 里 uniqued，ThreadSafeModule 必须拿同一个 context，
    // 否则违反 LLVM 契约（单线程下侥幸能跑，并发或升级后会 UAF）
    auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(_impl->llvm_ctx));
    if (auto err = _impl->jit->addIRModule(std::move(tsm))) {
        auto mod_err = "添加 IR 模块失败: " + llvm::toString(std::move(err));
        cleanup();
        return {ErrorCode::JIT_LINK_FAILED, -1, mod_err};
    }

    auto lookup = [this]<typename F>(const std::string &name) -> F {
        auto sym = _impl->jit->lookup(name);
        if (!sym) {
            llvm::consumeError(sym.takeError());
            return nullptr;
        }
        return sym->template toPtr<F>();
    };
    JitCompileResult r;
    r.tick = lookup.operator()<void (*)()>("circuit_tick");
    r.tick_cycle = lookup.operator()<void (*)(int)>("circuit_tick_cycle");
    r.tick_cycles = lookup.operator()<void (*)(int, uint64_t)>("circuit_tick_cycles");
    r.tick_batch = lookup.operator()<void (*)(uint64_t)>("circuit_tick_batch");
    r.tick_forever = lookup.operator()<void (*)()>("circuit_tick_forever");
    r.init = lookup.operator()<void (*)()>("circuit_init");
    r.deinit = lookup.operator()<void (*)()>("circuit_deinit");
    r.reset = lookup.operator()<void (*)()>("circuit_reset");
    r.set_wire = lookup.operator()<void (*)(int, const uint8_t *)>("circuit_set_wire");
    r.set_wire_u64 = lookup.operator()<void (*)(int, uint64_t)>("circuit_set_wire_u64");
    r.get_wire = lookup.operator()<void (*)(int, uint8_t *)>("circuit_get_wire");
    if (!r.tick || !r.init || !r.set_wire || !r.get_wire) {
        cleanup();
        return {ErrorCode::JIT_SYMBOL_FAILED, -1, "查找 JIT 符号失败"};
    }
    return r;
#endif
}

} // namespace dsc
