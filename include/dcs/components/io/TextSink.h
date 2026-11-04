// TextSink — 文本输出元件（print / fprint）共用的宿主回调
//
// 每个元件实例单独绑定，不改变其它实例的行为：同一个进程里不同的电路/窗口
// 可以各自接管自己那部分输出。回调在仿真线程里同步执行，必须在开始 tick
// 之前设置；owner 是元件本身，可用 name() 区分来源。
#pragma once

namespace dsc {

class Component;

struct TextSink {
    // data 不以 NUL 结尾，只在回调期间有效；len 是字节数。
    // ascii 模式的 print 每次交一个字符，十六进制模式每次交一整行（含换行）
    void (*on_text)(void *ctx, const Component *owner, const char *data, int len) = nullptr;
};

} // namespace dsc
