// ComponentFactory — 元件注册表
//
// 元件在各自的 .cpp 里通过 DCS_REGISTER_COMPONENT 自注册，这里只提供单例。
// 新增元件不需要改动本文件。
#include "dcs/ComponentFactory.h"

namespace dsc {

ComponentFactory &ComponentFactory::instance() {
    static ComponentFactory f;
    return f;
}

} // namespace dsc
