#include "shared.hpp"

namespace shared {

int shared_api() {
    return 23;
}

int unused_shared_api() {
    return 29;
}

__attribute__((visibility("default"))) int export_only_api() {
    return 30;
}

__attribute__((visibility("hidden"))) int hidden_implementation() {
    return 31;
}

} // namespace shared
