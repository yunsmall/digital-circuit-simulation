// Context 的实现：默认上下文用函数内静态对象，避免静态初始化顺序问题
#include "dcs/Context.h"

namespace dsc {

Context &Context::global() {
    static Context ctx;
    return ctx;
}

} // namespace dsc
