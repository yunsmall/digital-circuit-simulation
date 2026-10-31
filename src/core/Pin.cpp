#include "dcs/Pin.h"

namespace dsc {

Pin::Pin(const std::string &name, PinDir dir, int bit_width, Component *parent, bool is_tri_state, bool is_sequential) :
    _name(name), _dir(dir), _bit_width(bit_width), _parent(parent), _is_tri_state(is_tri_state),
    _is_sequential(is_sequential) {
}

} // namespace dsc
