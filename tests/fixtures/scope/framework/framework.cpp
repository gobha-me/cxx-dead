#include "framework.hpp"

void indexed_initializer_target();

int indexed_initialized_value = (indexed_initializer_target(), 1);

namespace framework {

void Application::run() {
    on_start();
}

void unused_framework() {}

} // namespace framework
