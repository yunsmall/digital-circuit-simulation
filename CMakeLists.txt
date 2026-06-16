cmake_minimum_required(VERSION 4.2)
project(digital_circuit_simulation)

set(CMAKE_CXX_STANDARD 23)

# ---- 后端选择 ----
# cmake -B build -DDCS_USE_TCC=ON
option(DCS_USE_TCC "Use TinyCC backend instead of LLVM/Clang" OFF)

if(DCS_USE_TCC)
    # TinyCC 后端
    if(NOT DEFINED TCC_ROOT)
        set(TCC_ROOT "E:/YeSmallWorkstation3/software_build/tinycc/win32")
    endif()
    message(STATUS "Using TCC backend: ${TCC_ROOT}")
    set(DCS_BACKEND_LIBS ${TCC_ROOT}/libtcc.lib)
    # libtcc.h 在 win32/libtcc/ 子目录，只加入这个目录
    # TCC 的 include/ 目录包含整套 C 标准库头文件，会与 MSVC 冲突，不能在 C++ 编译时加入
    # 运行时通过 tcc_set_lib_path 让 TCC 自己找到标准头文件
    set(DCS_BACKEND_INCLUDES ${TCC_ROOT}/libtcc)
    set(DCS_TCC_DLL "${TCC_ROOT}/libtcc.dll")
    add_compile_definitions(DCS_USE_TCC)
else()
    # LLVM/Clang 后端
    if(NOT DEFINED LLVM_ROOT)
        set(LLVM_ROOT "E:/YeSmallWorkstation3/clang+llvm-22.1.7-x86_64-pc-windows-msvc")
    endif()
    set(LLVM_DIR "${LLVM_ROOT}/lib/cmake/llvm")
    set(Clang_DIR "${LLVM_ROOT}/lib/cmake/clang")
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
find_package(cxxopts CONFIG REQUIRED)

# ---- 库（src/ 中所有 .cpp）----
file(GLOB DCS_SOURCES "src/*.cpp" "src/components/*.cpp" "src/util/*.cpp")
add_library(dcs_lib STATIC ${DCS_SOURCES})
target_include_directories(dcs_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${DCS_BACKEND_INCLUDES}
)
target_link_libraries(dcs_lib PUBLIC ${DCS_BACKEND_LIBS} nlohmann_json::nlohmann_json)

# ---- 运行时资源复制到输出目录 ----
# 挂在 dcs_lib 的 POST_BUILD 上，所有链接 dcs_lib 的目标自动获得。
# 用 $<TARGET_FILE_DIR:dcs_lib> 适配所有生成器（MSVC/Ninja/Makefile）。
set(_COPY_CMDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dcs_lib>/shim"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shim" "$<TARGET_FILE_DIR:dcs_lib>/shim"
)
if(DCS_USE_TCC)
    list(APPEND _COPY_CMDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DCS_TCC_DLL}" "$<TARGET_FILE_DIR:dcs_lib>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dcs_lib>/include"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TCC_ROOT}/include/tccdefs.h" "$<TARGET_FILE_DIR:dcs_lib>/include/tccdefs.h"
    )
endif()
add_custom_command(TARGET dcs_lib POST_BUILD ${_COPY_CMDS})

# ---- 主程序 ----
add_executable(digital_circuit_simulation main.cpp)
target_link_libraries(digital_circuit_simulation PRIVATE dcs_lib cxxopts::cxxopts)

# ---- CLI 工具 ----
add_executable(dcs_tool tools/dcs_tool.cpp)
target_link_libraries(dcs_tool PRIVATE dcs_lib cxxopts::cxxopts)

# ---- 测试（gtest + vcpkg）----
# 配置: cmake -B build -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()
    if(NOT DCS_USE_TCC)
        # MSVC + LLVM: 强制全局构造函数被链接
        foreach(_lib IN ITEMS LLVMSupport LLVMCore clangBasic)
            list(APPEND DCS_WHOLEARCHIVE "/WHOLEARCHIVE:${_lib}")
        endforeach()
    endif()

    function(add_dcs_test TEST_NAME TEST_SOURCE)
        add_executable(${TEST_NAME} ${TEST_SOURCE})
        target_link_libraries(${TEST_NAME} PRIVATE dcs_lib GTest::gtest GTest::gtest_main)
        set_target_properties(${TEST_NAME} PROPERTIES CXX_STANDARD 23)
        if(DCS_WHOLEARCHIVE)
            target_link_options(${TEST_NAME} PRIVATE ${DCS_WHOLEARCHIVE})
        endif()
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endfunction()

    add_dcs_test(test_gates       tests/test_gates.cpp)
    add_dcs_test(test_sequential  tests/test_sequential.cpp)
    add_dcs_test(test_circuit     tests/test_circuit.cpp)
    add_dcs_test(test_arithmetic  tests/test_arithmetic.cpp)
    add_dcs_test(test_composite   tests/test_composite.cpp)
    add_dcs_test(test_bitwidths   tests/test_bitwidths.cpp)
    add_dcs_test(test_dll         tests/test_dll.cpp)
    add_dcs_test(test_misc        tests/test_misc.cpp)
    add_dcs_test(test_json        tests/test_json.cpp)

    # 测试用 DLL 元件（8 位 AND 门）
    add_library(dll_test_and SHARED tests/dll_test_and.c)
    target_include_directories(dll_test_and PRIVATE ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(dll_test_and PROPERTIES C_STANDARD 11)
    # 复制到测试输出目录
    add_custom_command(TARGET test_dll POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:dll_test_and>" "$<TARGET_FILE_DIR:test_dll>"
    )
endif()
