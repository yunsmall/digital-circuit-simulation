# Verilator 集成测试
cmake_minimum_required(VERSION 4.2)

find_package(verilator CONFIG REQUIRED)

# 公共 include 路径
set(DCS_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# adder8 — 纯组合
add_library(adder8_dll SHARED wrap_adder8.cpp)
target_include_directories(adder8_dll PRIVATE ${DCS_INCLUDE})
set_target_properties(adder8_dll PROPERTIES CXX_STANDARD 17)
verilate(adder8_dll SOURCES adder8.v TOP_MODULE adder8)

# counter8 — 纯时序
add_library(counter8_dll SHARED wrap_counter8.cpp)
target_include_directories(counter8_dll PRIVATE ${DCS_INCLUDE})
set_target_properties(counter8_dll PROPERTIES CXX_STANDARD 17)
verilate(counter8_dll SOURCES counter8.v TOP_MODULE counter8)

# reg8 — 组合+时序混合
add_library(reg8_dll SHARED wrap_reg8.cpp)
target_include_directories(reg8_dll PRIVATE ${DCS_INCLUDE})
set_target_properties(reg8_dll PROPERTIES CXX_STANDARD 17)
verilate(reg8_dll SOURCES reg8.v TOP_MODULE reg8)

# 编译后自动复制 DLL 到 JSON 所在目录
foreach(_dll adder8_dll counter8_dll reg8_dll)
    add_custom_command(TARGET ${_dll} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${_dll}>" "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endforeach()
