cmake_minimum_required(VERSION 4.2)
project(digital_circuit_simulation)

set(CMAKE_CXX_STANDARD 23)

option(DCS_BUILD_EXECUTABLE "构建主程序 dcs" ON)
option(DCS_BUILD_TESTS "构建测试" ON)
option(DCS_BUILD_VERILOG "构建 Verilator 集成测试 DLL" ON)

# ---- 后端选择 ----
# cmake -B build -DDCS_USE_TCC=ON
option(DCS_USE_TCC "Use TinyCC backend instead of LLVM/Clang" OFF)

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
    add_compile_definitions(DCS_USE_TCC)
else()
    # LLVM/Clang 后端
    if(DEFINED LLVM_ROOT)
        set(LLVM_DIR "${LLVM_ROOT}/lib/cmake/llvm")
        set(Clang_DIR "${LLVM_ROOT}/lib/cmake/clang")
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

# ---- 库（src/ 中所有 .cpp）----
file(GLOB DCS_SOURCES "src/*.cpp" "src/components/*.cpp" "src/util/*.cpp")
add_library(dcs_lib STATIC ${DCS_SOURCES})
target_include_directories(dcs_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${DCS_BACKEND_INCLUDES}
)
target_link_libraries(dcs_lib PUBLIC ${DCS_BACKEND_LIBS} nlohmann_json::nlohmann_json)

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
    add_executable(dcs main.cpp)
    target_link_libraries(dcs PRIVATE dcs_lib cxxopts::cxxopts)
endif()

# ---- 测试（gtest + vcpkg）----
if(DCS_BUILD_TESTS)
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()

    function(add_dcs_test TEST_NAME TEST_SOURCE_GLOB)
        file(GLOB TEST_SOURCES ${TEST_SOURCE_GLOB})
        add_executable(${TEST_NAME} ${TEST_SOURCES})
        target_link_libraries(${TEST_NAME} PRIVATE dcs_lib GTest::gtest GTest::gtest_main)
        set_target_properties(${TEST_NAME} PROPERTIES CXX_STANDARD 23)
        if(DCS_WHOLEARCHIVE)
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
    add_dcs_test(test_dll         tests/test_dll/*.cpp)
    add_dcs_test(test_misc        tests/test_misc/*.cpp)
    add_dcs_test(test_json        tests/test_json/*.cpp)
    add_dcs_test(test_delay       tests/test_delay/*.cpp)
    add_dcs_test(test_floatpoint  tests/test_floatpoint/*.cpp)
    add_dcs_test(test_print       tests/test_print/*.cpp)
    add_dcs_test(test_signext     tests/test_signext/*.cpp)
    add_dcs_test(test_barrel      tests/test_barrel/*.cpp)
    add_dcs_test(test_prienc      tests/test_prienc/*.cpp)
    add_dcs_test(test_mpram       tests/test_mpram/*.cpp)
    add_dcs_test(test_fifo        tests/test_fifo/*.cpp)

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
endif()
