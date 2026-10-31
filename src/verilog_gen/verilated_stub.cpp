// Verilator 在不使用 SystemC 时约定由用户提供时间戳回调：verilated.cpp 以弱符号
// 引用 sc_time_stamp()，而 Windows 的 COFF 不支持弱符号，不给定义就直接链接失败。
// 本仿真不做时间模型，时间恒为 0。
double sc_time_stamp() {
    return 0;
}
