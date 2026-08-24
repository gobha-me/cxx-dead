#include "../../scope_external/opaque.hpp"
#include "../framework/framework.hpp"

void live_helper() {}
void dead_helper() {}

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
