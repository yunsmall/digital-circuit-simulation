// Verilator 在不使用 SystemC 时约定由用户提供时间戳回调：verilated.cpp 以弱符号
// 引用 sc_time_stamp()，而 Windows 的 COFF 不支持弱符号，不给定义就直接链接失败。
// 本仿真不做时间模型，时间恒为 0。
//
// 故意不加 extern "C"：Verilator 头文件里的声明是 C++ 链接的
// （verilated_funcs.h: extern double sc_time_stamp() VL_ATTR_WEAK），弱符号名
// 也是 mangled 的 _Z13sc_time_stampv。改成 C 链接反而对不上，别"顺手修正"。
double sc_time_stamp() {
    return 0;
}
