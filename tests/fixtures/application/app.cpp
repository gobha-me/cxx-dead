#include "shared.hpp"

void live_helper();

namespace live {

void leaf() {}

void run() {
    leaf();
    live_helper();
}

} // namespace live

void dead_b();

void dead_a() {
    dead_b();
}

void dead_b() {
    dead_a();
}

class LegacyParser {
  public:
    void parse();
    void reset();
};

void LegacyParser::parse() {
    reset();
}

void LegacyParser::reset() {}

using Callback = void (*)();

void register_callback(Callback) {}

void escaped_callback() {}

int initialize_global() {
    return 42;
}

int initialized_value = initialize_global();

struct Renderer {
    virtual ~Renderer() = default;
    virtual void render();
};

void Renderer::render() {}

struct ConcreteRenderer : Renderer {
    ConcreteRenderer() = default;
    void render() override;
};

void ConcreteRenderer::render() {}

int main() {
    live::run();
    register_callback(&escaped_callback);
    using ConcreteAlias = ConcreteRenderer;
    ConcreteAlias renderer;
    Renderer* base = &renderer;
    base->render();
}
