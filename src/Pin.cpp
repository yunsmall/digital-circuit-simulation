#include "dcs/Pin.h"

namespace dsc {

Pin::Pin(const std::string &name, PinDir dir, int bit_width, Component *parent) :
    _name(name), _dir(dir), _bit_width(bit_width), _parent(parent) {
}

} // namespace dsc
