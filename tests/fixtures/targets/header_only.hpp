#pragma once

namespace header_only {

template <typename Value> Value identity(Value value) {
    return value;
}

} // namespace header_only
