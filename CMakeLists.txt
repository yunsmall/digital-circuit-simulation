cmake_minimum_required(VERSION 4.2)
project(digital_circuit_simulation VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)

option(DCS_BUILD_EXECUTABLE "构建主程序 dcs" ON)
option(DCS_BUILD_TESTS "构建测试" ON)
option(DCS_BUILD_VERILOG "构建 Verilator 集成测试 DLL" ON)
option(DCS_BUILD_BENCHMARK "构建性能基准 dcs_bench" ON)

# ---- 后端选择 ----
# cmake -B build -DDCS_USE_TCC=ON
option(DCS_USE_TCC "Use TinyCC backend instead of LLVM/Clang" OFF)
# guest 通过 virtio-net 访问宿主和互联网：用 libslirp 做用户态 NAT，
# 宿主零配置、不需要驱动；默认关闭，避免给不需要网络的构建引入依赖
option(DCS_BUILD_NET "启用 libslirp 用户态网络后端" OFF)

if(DCS_USE_TCC)
    # TinyCC 后端
    if(DEFINED TCC_ROOT)
        message(STATUS "Using TCC backend: ${TCC_ROOT}")
    else()
        message(STATUS "Using TCC backend: (系统搜索)")
    endif()
    if(WIN32)
        if(DEFINED TCC_ROOT)
            # Windows: TCC_ROOT 指向 win32/ 目录（含 libtcc.lib、libtcc.dll、libtcc/ 子目录）
            set(DCS_BACKEND_LIBS ${TCC_ROOT}/libtcc.lib)
            set(DCS_BACKEND_INCLUDES ${TCC_ROOT}/libtcc)
            set(DCS_TCC_DLL "${TCC_ROOT}/libtcc.dll")
        else()
            # 未设置 TCC_ROOT：尝试系统搜索
            find_library(TCC_LIBRARY tcc)
            find_path(TCC_INCLUDE_DIR libtcc.h)
            if(NOT TCC_LIBRARY OR NOT TCC_INCLUDE_DIR)
                message(FATAL_ERROR "找不到 libtcc，请通过 -DTCC_ROOT=<路径> 指定")
            endif()
            set(DCS_BACKEND_LIBS ${TCC_LIBRARY})
            set(DCS_BACKEND_INCLUDES ${TCC_INCLUDE_DIR})
        endif()
    else()
        # Linux
        if(DEFINED TCC_ROOT)
            # 优先搜索 TCC_ROOT，再回退系统路径
            find_library(TCC_LIBRARY tcc PATHS ${TCC_ROOT}/lib ${TCC_ROOT}/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
            if(NOT TCC_LIBRARY)
                find_library(TCC_LIBRARY tcc PATHS ${TCC_ROOT}/lib ${TCC_ROOT}/lib/x86_64-linux-gnu)
            endif()
            find_path(TCC_INCLUDE_DIR libtcc.h PATHS ${TCC_ROOT}/include NO_DEFAULT_PATH)
            if(NOT TCC_INCLUDE_DIR)
                find_path(TCC_INCLUDE_DIR libtcc.h PATHS ${TCC_ROOT}/include)
            endif()
        else()
            find_library(TCC_LIBRARY tcc)
            find_path(TCC_INCLUDE_DIR libtcc.h)
        endif()
        if(NOT TCC_LIBRARY OR NOT TCC_INCLUDE_DIR)
            message(FATAL_ERROR "找不到 libtcc，请通过 -DTCC_ROOT=<路径> 指定")
        endif()
        set(DCS_BACKEND_LIBS ${TCC_LIBRARY})
        set(DCS_BACKEND_INCLUDES ${TCC_INCLUDE_DIR})
    endif()
else()
    # LLVM/Clang 后端
    if(DEFINED LLVM_ROOT)
        set(LLVM_DIR "${LLVM_ROOT}/lib/cmake/llvm")
        set(Clang_DIR "${LLVM_ROOT}/lib/cmake/clang")
    endif()
    # 官方 CI 构建的 LLVM 包在 LLVMExports.cmake 里按 CI 机器的路径引用 DIA SDK 和
    # zstd 的导入目标，本机路径不同时 generate 会直接失败。修改 LLVM 安装目录在升级
    # LLVM 之后会被一并删掉，所以在这里按本机环境补出同名目标。
    if(WIN32)
        if(NOT TARGET DIASDK::Diaguids)
            if(NOT MSVC_DIA_SDK_DIR)
                if(DEFINED ENV{VSINSTALLDIR})
                    set(MSVC_DIA_SDK_DIR "$ENV{VSINSTALLDIR}DIA SDK")
                else()
                    # 环境变量名里的括号必须转义，否则 CMake 解析到 ")" 就报语法错误
                    set(DCS_PF86 "$ENV{ProgramFiles\(x86\)}")
                    find_program(DCS_VSWHERE vswhere HINTS "${DCS_PF86}/Microsoft Visual Studio/Installer")
                    if(DCS_VSWHERE)
                        execute_process(COMMAND "${DCS_VSWHERE}" -latest -property installationPath
                                        OUTPUT_VARIABLE DCS_VS_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
                        if(DCS_VS_DIR)
                            set(MSVC_DIA_SDK_DIR "${DCS_VS_DIR}/DIA SDK")
                        endif()
                    endif()
                endif()
            endif()
            if(MSVC_DIA_SDK_DIR AND EXISTS "${MSVC_DIA_SDK_DIR}/include/dia2.h")
                add_library(DIASDK::Diaguids UNKNOWN IMPORTED)
                set_target_properties(DIASDK::Diaguids PROPERTIES
                    IMPORTED_LOCATION "${MSVC_DIA_SDK_DIR}/lib/amd64/diaguids.lib"
                    INTERFACE_INCLUDE_DIRECTORIES "${MSVC_DIA_SDK_DIR}/include")
            endif()
        endif()
        if(NOT TARGET zstd::libzstd_static)
            find_library(DCS_ZSTD_LIB NAMES zstd libzstd)
            if(DCS_ZSTD_LIB)
                add_library(zstd::libzstd_static UNKNOWN IMPORTED)
                set_target_properties(zstd::libzstd_static PROPERTIES IMPORTED_LOCATION "${DCS_ZSTD_LIB}")
            endif()
        endif()
    elseif(UNIX)
        # 发行版 LLVM 的导出文件引用 zstd::libzstd_shared（系统 zstd 包的 target 名），
        # 而 vcpkg 的 zstd 只导出 zstd::libzstd_static。先把 vcpkg 那份找出来，
        # 再补一个同名别名，否则 LLVMConfig.cmake 加载时直接报 target 不存在
        find_package(zstd CONFIG QUIET)
        if(TARGET zstd::libzstd_static AND NOT TARGET zstd::libzstd_shared)
            add_library(zstd::libzstd_shared ALIAS zstd::libzstd_static)
        endif()
    endif()
    find_package(LLVM REQUIRED CONFIG)
    find_package(Clang REQUIRED CONFIG)
    message(STATUS "Using LLVM backend: ${LLVM_PACKAGE_VERSION}")
    set(DCS_BACKEND_LIBS
        clangFrontend clangCodeGen clangDriver clangParse clangSema
        clangSerialization clangAnalysis clangAST clangBasic clangLex
        clangEdit clangRewrite clangRewriteFrontend
        LLVMOrcJIT LLVMOrcDebugging LLVMOrcShared LLVMOrcTargetProcess
        LLVMX86CodeGen LLVMX86Desc LLVMX86Info LLVMX86AsmParser LLVMX86Disassembler
        LLVMCore LLVMSupport LLVMIRReader LLVMMCJIT LLVMExecutionEngine
        LLVMRuntimeDyld LLVMTarget LLVMMC LLVMMCParser LLVMObject
        LLVMBitReader LLVMBitWriter LLVMBitstreamReader LLVMPasses
        LLVMTransformUtils LLVMAnalysis LLVMScalarOpts LLVMInstCombine
        LLVMAggressiveInstCombine LLVMipo LLVMVectorize LLVMCodeGen
        LLVMCodeGenTypes LLVMAsmPrinter LLVMProfileData LLVMDebugInfoDWARF
        LLVMLinker LLVMBinaryFormat LLVMInstrumentation LLVMTargetParser
    )
    set(DCS_BACKEND_INCLUDES ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    add_compile_definitions(${LLVM_DEFINITIONS})
endif()

# ---- 第三方库 ----
find_package(nlohmann_json CONFIG REQUIRED)
# ELF 映像解析用 header-only 的 elfio：不依赖仿真后端，TCC 配置下同样可用
find_package(elfio CONFIG REQUIRED)

# ---- 库（核心、元件、工具三部分）----
# src/bin 是各个可执行文件的入口，不参与库：测试目标链接 gtest_main，
# 库里再来一个 main 会在链接时抢掉它
file(GLOB DCS_SOURCES CONFIGURE_DEPENDS "src/core/*.cpp" "src/components/*/*.cpp" "src/util/*.cpp")

# JIT 需要的 shim 头文件内容直接编进可执行文件：运行时不再需要 include 目录，
# TCC 的 add_include_path 和路径探测逻辑都可以省掉
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shim/dcs_stdint.h DCS_SHIM_STDINT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shim/dcs_stdbool.h DCS_SHIM_STDBOOL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shim/dcs_util.h DCS_SHIM_UTIL)
set(DCS_SHIM_SOURCE "${DCS_SHIM_STDINT}\n${DCS_SHIM_STDBOOL}\n${DCS_SHIM_UTIL}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/shim_embedded.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/dcs_shim_embedded.h @ONLY)
# LTO：宿主侧代码（解释器、代码生成、元件实现）跨编译单元内联。
# JIT 生成的热点代码不经过这里，所以只影响启动和 C++ 侧开销；
# 链接 90MB 的 LLVM 静态库会明显变慢，默认关闭
option(DCS_ENABLE_LTO "启用 LTO（链接时间明显变长）" OFF)
if(DCS_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT DCS_IPO_OK LANGUAGES CXX)
    if(DCS_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
endif()

# 共享库能省磁盘：LLVM 静态链进 dcs_lib 一份，不必每个 exe 复制 90MB。
# 但官方 LLVM 发行包用静态 CRT（/MT）编译，共享库必须配 /MD，链接器会报
# /failifmismatch: RuntimeLibrary。只有自己用 /MD 重编 LLVM 时才能打开这个选项。
option(DCS_SHARED_LIB "把 dcs_lib 构建为共享库（要求 /MD 编译的 LLVM，官方包不满足）" OFF)
if(DCS_SHARED_LIB)
    # 共享库必须配动态 CRT：Circuit 和 Component 对象跨 DLL 分配/释放，
    # 静态 CRT 会让 exe 和 DLL 各有一个堆，跨边界 delete 直接破坏堆。
    # 用 WIN32 判断而不是 MSVC：clang++ 的编译器 ID 是 Clang，MSVC 变量为假，
    # 但它同样走 MSVC ABI 和静态 CRT 默认值
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    add_library(dcs_lib SHARED ${DCS_SOURCES})
    set(DCS_LIB_LINK dcs_lib)
    if(WIN32)
        # 自动导出本库定义的所有符号，免去给每个类手写 __declspec(dllexport)。
        # .def 里会连带导出静态链接进来的 LLVM 符号，链接稍慢但功能正确
        set_target_properties(dcs_lib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
else()
    add_library(dcs_lib STATIC ${DCS_SOURCES})
    # 元件靠静态初始化器自注册（DCS_REGISTER_COMPONENT）。静态库里没有被其它
    # 目标引用的 .obj 会被链接器整个丢掉，注册对象也就不会执行，表现为
    # --list-types 一个元件都不剩。WHOLE_ARCHIVE 生成表达式由 CMake 按工具链
    # 展开成 /WHOLEARCHIVE（MSVC/clang-cl）或 --whole-archive（GCC/Clang）
    set(DCS_LIB_LINK "$<LINK_LIBRARY:WHOLE_ARCHIVE,dcs_lib>")
endif()
target_include_directories(dcs_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${DCS_BACKEND_INCLUDES}
)
# 后端库用 PRIVATE：它们已经静态链进 dcs_lib（或表现为运行时依赖的 TCC DLL），
# 可执行文件只通过 dcs_lib 的接口调用，不必再链一遍——否则共享库省下的体积又还回去了
target_link_libraries(dcs_lib PRIVATE ${DCS_BACKEND_LIBS})
target_link_libraries(dcs_lib PUBLIC nlohmann_json::nlohmann_json elfio::elfio)
if(DCS_USE_TCC)
    target_compile_definitions(dcs_lib PRIVATE DCS_USE_TCC)
endif()

# ---- 运行时资源复制到输出目录 ----
# 挂在 dcs_lib 的 POST_BUILD 上，所有链接 dcs_lib 的目标自动获得。
# 用 $<TARGET_FILE_DIR:dcs_lib> 适配所有生成器（MSVC/Ninja/Makefile）。
set(_COPY_CMDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dcs_lib>/shim"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shim" "$<TARGET_FILE_DIR:dcs_lib>/shim"
)
if(DCS_USE_TCC AND WIN32 AND DEFINED TCC_ROOT AND DCS_TCC_DLL)
    list(APPEND _COPY_CMDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DCS_TCC_DLL}" "$<TARGET_FILE_DIR:dcs_lib>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TCC_ROOT}/include/tccdefs.h" "$<TARGET_FILE_DIR:dcs_lib>/shim/tccdefs.h"
    )
endif()
add_custom_command(TARGET dcs_lib POST_BUILD ${_COPY_CMDS})

# ---- 主程序（合并 simulator + tool）----
if(DCS_BUILD_EXECUTABLE)
    find_package(cxxopts CONFIG REQUIRED)
    add_executable(dcs src/bin/main.cpp)
    target_link_libraries(dcs PRIVATE ${DCS_LIB_LINK} cxxopts::cxxopts)
    if(DCS_USE_TCC)
        target_compile_definitions(dcs PRIVATE DCS_USE_TCC)
    endif()

    # 整机仿真入口：加载 RISC-V ELF、串口接宿主键盘、按 tohost 判定结果。
    # 这些是计算机仿真特有的东西，单独一个可执行文件，不往纯电路仿真的 dcs 里塞
    add_executable(dcs_soc src/bin/soc_main.cpp)
    target_link_libraries(dcs_soc PRIVATE ${DCS_LIB_LINK} cxxopts::cxxopts)
    if(DCS_USE_TCC)
        target_compile_definitions(dcs_soc PRIVATE DCS_USE_TCC)
    endif()
    if(DCS_BUILD_NET)
        # libslirp 在 vcpkg 里只提供 pkg-config 模块（vcpkg print-usage libslirp）
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SLIRP REQUIRED IMPORTED_TARGET slirp)
        # 两个可执行文件都要能加载带 virtio_net 的电路（--json-to-c / 纯电路仿真
        # 也要能认出 backend=slirp），后端只编进可执行文件，不污染 dcs_lib
        target_sources(dcs PRIVATE src/net/SlirpBackend.cpp)
        target_link_libraries(dcs PRIVATE PkgConfig::SLIRP)
        target_sources(dcs_soc PRIVATE src/net/SlirpBackend.cpp)
        target_link_libraries(dcs_soc PRIVATE PkgConfig::SLIRP)
        # slirp 的 socket 调用在 Windows 上要显式链 ws2_32；MSVC 的隐式链接
        # 在 clang + lld 下不生效，会报 WSAStartup/socket 等符号未定义
        if(WIN32)
            target_link_libraries(dcs PRIVATE ws2_32)
            target_link_libraries(dcs_soc PRIVATE ws2_32)
        endif()
    endif()
endif()

# ---- 性能基准（不注册进 ctest，手动运行）----
if(DCS_BUILD_BENCHMARK)
    add_executable(dcs_bench benchmarks/benchmark.cpp)
    target_link_libraries(dcs_bench PRIVATE ${DCS_LIB_LINK})
    set_target_properties(dcs_bench PROPERTIES CXX_STANDARD 23)
endif()

# ---- 测试（gtest + vcpkg）----
if(DCS_BUILD_TESTS)
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()

    function(add_dcs_test TEST_NAME TEST_SOURCE_GLOB)
        file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${TEST_SOURCE_GLOB})
        add_executable(${TEST_NAME} ${TEST_SOURCES})
        target_link_libraries(${TEST_NAME} PRIVATE ${DCS_LIB_LINK} GTest::gtest GTest::gtest_main)
        # tests/common 下是跨测试复用的辅助件（如 Wishbone 主设备测试台），
        # 各测试用 #include "common/xxx.h" 引用
        target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        set_target_properties(${TEST_NAME} PROPERTIES CXX_STANDARD 23)
        # whole-archive 只对静态库有意义：共享库的符号本来就全部保留
        if(NOT DCS_SHARED_LIB AND DCS_WHOLEARCHIVE)
            target_link_options(${TEST_NAME} PRIVATE ${DCS_WHOLEARCHIVE})
        endif()
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endfunction()

    add_dcs_test(test_gates       tests/test_gates/*.cpp)
    add_dcs_test(test_sequential  tests/test_sequential/*.cpp)
    add_dcs_test(test_circuit     tests/test_circuit/*.cpp)
    add_dcs_test(test_arithmetic  tests/test_arithmetic/*.cpp)
    add_dcs_test(test_composite   tests/test_composite/*.cpp)
    add_dcs_test(test_bitwidths   tests/test_bitwidths/*.cpp)
    add_dcs_test(test_width128    tests/test_width128/*.cpp)
    add_dcs_test(test_dll         tests/test_dll/*.cpp)
    add_dcs_test(test_misc        tests/test_misc/*.cpp)
    add_dcs_test(test_json        tests/test_json/*.cpp)
    # 蓝图库测试需要按源码树里的 library/ 加载真实蓝图文件
    add_dcs_test(test_library     tests/library/*.cpp)
    target_compile_definitions(test_library PRIVATE DCS_LIBRARY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/library")
    add_dcs_test(test_delay       tests/test_delay/*.cpp)
    add_dcs_test(test_floatpoint  tests/test_floatpoint/*.cpp)
    add_dcs_test(test_print       tests/test_print/*.cpp)
    add_dcs_test(test_signext     tests/test_signext/*.cpp)
    add_dcs_test(test_barrel      tests/test_barrel/*.cpp)
    add_dcs_test(test_prienc      tests/test_prienc/*.cpp)
    add_dcs_test(test_mpram       tests/test_mpram/*.cpp)
    add_dcs_test(test_fifo        tests/test_fifo/*.cpp)
    add_dcs_test(test_riscv       tests/test_riscv/*.cpp)
    add_dcs_test(test_soc         tests/test_soc/*.cpp)
    add_dcs_test(test_riscv64     tests/test_riscv64/*.cpp)
    set_tests_properties(test_riscv64 PROPERTIES ENVIRONMENT
        "DCS_RISCV64_ELF_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/riscv-official/out64")
    # 官方 riscv-tests 的 ELF 目录必须传给测试：默认值是相对路径，
    # 从构建目录运行时找不到，official.cpp 会静默 GTEST_SKIP()，
    # 结果看起来全绿、实际上没验证任何指令
    set_tests_properties(test_riscv PROPERTIES ENVIRONMENT
        "DCS_RISCV_ELF_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/riscv-official/out")
    add_dcs_test(test_plic       tests/test_plic/*.cpp)
    add_dcs_test(test_uart       tests/test_uart/*.cpp)
    add_dcs_test(test_virtio     tests/test_virtio/*.cpp)
    add_dcs_test(test_uart_term  tests/test_uart_term/*.cpp)
    add_dcs_test(test_uart_in    tests/test_uart_in/*.cpp)
    add_dcs_test(test_uart_feed  tests/test_uart_feed/*.cpp)
    add_dcs_test(test_rtc        tests/test_rtc/*.cpp)
    add_dcs_test(test_gpio       tests/test_gpio/*.cpp)
    add_dcs_test(test_pwm        tests/test_pwm/*.cpp)
    add_dcs_test(test_watchdog   tests/test_watchdog/*.cpp)
    add_dcs_test(test_reg_bridge tests/test_reg_bridge/*.cpp)
    add_dcs_test(test_spi        tests/test_spi/*.cpp)
    add_dcs_test(test_i2c        tests/test_i2c/*.cpp)
    add_dcs_test(test_cache      tests/test_cache/*.cpp)
    add_dcs_test(test_dcache     tests/test_dcache/*.cpp)
    add_dcs_test(test_dma        tests/test_dma/*.cpp)
    add_dcs_test(test_axi        tests/test_axi/*.cpp)
    add_dcs_test(test_edgecases  tests/test_edgecases/*.cpp)
    add_dcs_test(test_coverage   tests/test_coverage/*.cpp)
    add_dcs_test(test_wbrom      tests/test_wbrom/*.cpp)
    add_dcs_test(test_wishbone_ic tests/test_wishbone_ic/*.cpp)
    add_dcs_test(test_alu        tests/test_alu/*.cpp)
    add_dcs_test(test_bcd2seg    tests/test_bcd2seg/*.cpp)
    add_dcs_test(test_display    tests/test_display/*.cpp)
    add_dcs_test(test_sync2ff    tests/test_sync2ff/*.cpp)
    add_dcs_test(test_debounce   tests/test_debounce/*.cpp)
    add_dcs_test(test_assert     tests/test_assert/*.cpp)
    add_dcs_test(test_parity     tests/test_parity/*.cpp)
    add_dcs_test(test_regfile    tests/test_regfile/*.cpp)
    add_dcs_test(test_wallclock  tests/test_wallclock/*.cpp)
    add_dcs_test(test_fpu_flags  tests/test_fpu_flags/*.cpp)
    add_dcs_test(test_adcdac     tests/test_adcdac/*.cpp)
    add_dcs_test(test_ringcounter tests/test_ringcounter/*.cpp)
    add_dcs_test(test_async_fifo  tests/test_async_fifo/*.cpp)
    add_dcs_test(test_truth_table tests/test_truth_table/*.cpp)
    add_dcs_test(test_wbmem      tests/test_wbmem/*.cpp)
    add_dcs_test(test_rvc        tests/test_rvc/*.cpp)
    # Verilator 集成测试：DLL 由 verilog_test/ 子项目生成，测试里用的是相对路径，
    # 所以工作目录要指到那里，否则加载不到 DLL
    if(DCS_BUILD_VERILOG)
        add_dcs_test(test_verilog tests/test_verilog/*.cpp)
        set_tests_properties(test_verilog PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/verilog_test")
    endif()

    # 测试用 DLL 元件（8 位 AND 门）
    add_library(dll_test_and SHARED tests/dll_test_and.c)
    target_include_directories(dll_test_and PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_target_properties(dll_test_and PROPERTIES C_STANDARD 11)
    # 复制到测试输出目录
    add_custom_command(TARGET test_dll POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:dll_test_and>" "$<TARGET_FILE_DIR:test_dll>"
    )
endif()
endif()

# ---- 安装规则 ----
include(GNUInstallDirs)
install(TARGETS dcs_lib
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/dcs/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dcs
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY shim/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dcs/shim
)
install(DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dcs/examples
)
if(DCS_BUILD_EXECUTABLE)
    install(TARGETS dcs DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
# Windows TCC：运行时 DLL 也需安装到 bin/
if(DCS_USE_TCC AND WIN32 AND DEFINED TCC_ROOT AND DCS_TCC_DLL)
    install(FILES "${DCS_TCC_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${TCC_ROOT}/include/tccdefs.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dcs/shim)
endif()

# ---- Verilator 集成（可选）----
if(DCS_BUILD_VERILOG)
    add_subdirectory(verilog_test)

    # tools/verilog2dcs.py 生成的元件：verilog_gen/<top>/ 放 Verilog 源和
    # dcs_component.txt（记录元件类名）。元件 cpp 必须和模型编进同一个 target，
    # 才能拿到 verilate() 生成的 V<top>.h 的头文件路径
    file(GLOB _dcs_vgen_dirs RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/verilog_gen"
         "${CMAKE_CURRENT_SOURCE_DIR}/verilog_gen/*")
    set(_dcs_vgen_comps "")
    set(_dcs_vgen_tops "")
    foreach(_d ${_dcs_vgen_dirs})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/verilog_gen/${_d}/dcs_component.txt")
            file(READ "${CMAKE_CURRENT_SOURCE_DIR}/verilog_gen/${_d}/dcs_component.txt" _comp)
            string(STRIP "${_comp}" _comp)
            list(APPEND _dcs_vgen_comps
                 "${CMAKE_CURRENT_SOURCE_DIR}/src/verilog_gen/${_comp}.cpp")
            list(APPEND _dcs_vgen_tops "${_d}")
        endif()
    endforeach()

    # 所有模型和元件 cpp 编进同一个 target：verilate() 会把 verilated.cpp 这类
    # 运行时源文件塞进 target，一个模型一个 target 就会重复定义一堆符号。
    # 同一个 target 里 CMake 按路径去重，运行时只编一次
    if(_dcs_vgen_comps)
        add_library(dcs_verilog_models OBJECT
            ${_dcs_vgen_comps}
            "${CMAKE_CURRENT_SOURCE_DIR}/src/verilog_gen/verilated_stub.cpp")
        target_include_directories(dcs_verilog_models PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_BINARY_DIR}/generated")
        foreach(_d ${_dcs_vgen_tops})
            file(GLOB _dcs_vsrcs "${CMAKE_CURRENT_SOURCE_DIR}/verilog_gen/${_d}/*.v")
            verilate(dcs_verilog_models SOURCES ${_dcs_vsrcs}
                     TOP_MODULE ${_d} PREFIX V${_d}
                     VERILATOR_ARGS --pins-inout-enables)
        endforeach()
        target_sources(dcs_lib PRIVATE $<TARGET_OBJECTS:dcs_verilog_models>)
    endif()
endif()
