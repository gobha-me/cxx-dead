#include "../../scope_external/opaque.hpp"
#include "../framework/framework.hpp"

void live_helper() {}
void dead_helper() {}
void indexed_initializer_target() {}
void external_initializer_target() {}

void local_static_initializer_target() {}

void unused_local_static_owner() {
    static int local_value = (local_static_initializer_target(), 1);
    static_cast<void>(local_value);
}

class NullVectorApp final : public framework::Application {
  public:
    void on_start() override {
        live_helper();
        external_api();
    }
};

int main() {
    NullVectorApp app;
    app.run();
}
