// JitBackend — 把生成的 C 源码编译成可调用代码，并解析入口函数指针
//
// 这里只做"编译与符号解析"，不碰网表、不生成代码、不参与 tick。
// tick 走的是编译后解析出的函数指针，所以本类的方法只在 Circuit::compile() 里调用一次，
// 不会给每 tick 的路径增加任何间接层。
#include "dcs/JitBackend.h"

#include <atomic>
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
// 默认 inline：生成的元件函数都是普通 static 函数、没有任何强制内联属性，
// 交给 LLVM 的成本模型自己决定——小函数（线网搬运包装、常量、CPU 包装）会
// 展开，大的元件函数保持独立。实测整机 5 亿拍比只跑 basic 快约 3%。
// basic 只做 mem2reg/instcombine/gvn/sccp/dce 这类纯精简 pass；none 用于排查
// 生成代码本身的问题。注意：元件函数一旦重新加回 always_inline，inline 模式
// 会把 circuit_tick 撑成巨型函数，icache 挤爆后会掉 5~15%
void runJitPasses(llvm::Module &m) {
    const char *set = std::getenv("DCS_JIT_PASS_SET");
    const std::string_view mode = set ? std::string_view(set) : std::string_view("inline");
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
    // 队列头尾的原子读写：TCC 的 JIT 没有系统原子运行时，和 memset 一样由宿主提供
    {
        static auto s_atomic_load = [](const void *p) -> unsigned int {
            std::atomic_ref<unsigned int> ref(*const_cast<unsigned int *>(static_cast<const unsigned int *>(p)));
            return ref.load(std::memory_order_acquire);
        };
        static auto s_atomic_store = [](void *p, unsigned int v) {
            std::atomic_ref<unsigned int> ref(*static_cast<unsigned int *>(p));
            ref.store(v, std::memory_order_release);
        };
        tcc_add_symbol(s, "dcs_atomic_load_acq", reinterpret_cast<void *>(+s_atomic_load));
        tcc_add_symbol(s, "dcs_atomic_store_rel", reinterpret_cast<void *>(+s_atomic_store));
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
    // 空 CPU = 目标架构的默认基线。写死 x86-64 会让 ARM64 宿主直接失败；
    // 用基线也避免 CI 虚拟机不支持宿主新指令时 SIGILL
    invoc.getTargetOpts().CPU = "";
    invoc.getLangOpts().C99 = true;
    invoc.getLangOpts().C11 = true;
    invoc.getLangOpts().CPlusPlus = false;
    invoc.getLangOpts().Bool = true;
    invoc.getLangOpts().Freestanding = true; // 不依赖标准库
    // 前端会按 O2 跑完整的 IR 优化流水线（含内联），生成的元件函数绝大多数被卷进
    // circuit_tick；runJitPasses 再按 DCS_JIT_PASS_SET 补一轮。
    // 用 DisableLLVMPasses 关掉前端优化能让元件函数保持独立，但整机慢十倍，别这么干
    invoc.getCodeGenOpts().OptimizationLevel = 2;
    invoc.getCodeGenOpts().RelocationModel = llvm::Reloc::PIC_;
    // 调试用：带行号信息生成 IR，配合 DCS_JIT_IR_DUMP 能把 perf 采到的地址
    // 还原成生成代码里的元件
    if (std::getenv("DCS_JIT_DEBUG"))
        invoc.getCodeGenOpts().setDebugInfo(llvm::codegenoptions::FullDebugInfo);

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
        // action 可能持有挂在 llvm_ctx 上的 IR 对象，先放掉再销毁上下文
        action.reset();
        cleanup();
        return {ErrorCode::JIT_COMPILE_FAILED, -1, "Clang 编译 C 代码失败"};
    }
    auto module = action->takeModule();
    if (!module) {
        // ExecuteAction 成功但拿不到模块属于后端内部状态异常，不能解引用空指针
        action.reset();
        cleanup();
        return {ErrorCode::JIT_COMPILE_FAILED, -1, "Clang 没有返回 IR 模块"};
    }
    runJitPasses(*module);

    // 调试用：把 JIT 实际拿去编译的 IR 落到文件里。JIT 出来的机器码没有符号表，
    // 拿这份 IR 用同样的参数离线再编一遍就能对上函数边界，perf 采样才好归因
    if (const char *ir_path = std::getenv("DCS_JIT_IR_DUMP")) {
        std::error_code ec;
        llvm::raw_fd_ostream out(ir_path, ec);
        if (!ec)
            module->print(out, nullptr);
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        // module 析构要碰它所属的 LLVMContext，必须先放掉 module 再 cleanup
        module.reset();
        cleanup();
        return {ErrorCode::JIT_LINK_FAILED, -1, "检测目标机失败: " + llvm::toString(jtmb.takeError())};
    }
    // detectHost 会按宿主 CPU 填 CPU；显式清空改成该架构的通用基线，
    // 否则 A 机器编出来的代码到 B 机器上可能用不了
    jtmb->setCPU("");
    auto jit_or_err = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit_or_err) {
        auto llvm_err = "创建 LLJIT 失败: " + llvm::toString(jit_or_err.takeError());
        module.reset();
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
            // module 还没交给 ThreadSafeModule，cleanup 会销毁它依赖的 LLVMContext
            module.reset();
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
