#include "dcs/Pin.h"

#include <cassert>

namespace dsc {

Pin::Pin(const std::string &name, PinDir dir, int bit_width, Component *parent, bool is_tri_state, bool is_sequential,
         PinEdge edge) :
    _name(name), _dir(dir), _bit_width(bit_width), _parent(parent), _is_tri_state(is_tri_state),
    _is_sequential(is_sequential), _edge(edge) {
    // 上升/下降沿只对 1 位信号有定义；多位信号只能声明 Change
    assert((edge != PinEdge::Rising && edge != PinEdge::Falling) || bit_width == 1);
    // 边沿依赖只属于输入引脚
    assert(edge == PinEdge::None || isInput());
}

} // namespace dsc
