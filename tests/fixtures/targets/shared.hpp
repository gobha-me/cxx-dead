#pragma once

namespace shared {

__attribute__((visibility("default"))) int shared_api();
__attribute__((visibility("default"))) int unused_shared_api();

template <typename Value> Value public_identity(Value value) {
    return value;
}

static int private_header_helper() {
    return 37;
}

} // namespace shared
