#include "shared.hpp"

void cross_tu_live();
void generated_live();

namespace overloads {

void select(int) {}
void select(double) {}

void run() {
    select(1);
}

} // namespace overloads

namespace alpha {

void same_name() {}

} // namespace alpha

namespace beta {

void same_name() {}

} // namespace beta

static void internal_live() {}
static void internal_dead() {}

namespace {

void anonymous_live() {}
void anonymous_dead() {}

} // namespace

void external_dead() {}

struct LiveBase {
    LiveBase() {}
    virtual ~LiveBase() {}
    virtual void render() {}
    virtual void unused_virtual() {}
};

struct LiveMember {
    LiveMember() {}
    ~LiveMember() {}
};

struct LiveAggregate : LiveBase {
    LiveAggregate() {}
    ~LiveAggregate() override {}
    void render() override {}
    void unused_virtual() override {}
    LiveMember member;
};

struct DeadAggregate {
    DeadAggregate() {}
    ~DeadAggregate() {}
};

using Callback = void (*)();

void invoke_callback_directly() {}
void escaped_callback() {}
void unused_callback() {}

void register_callback(Callback) {}

namespace templates {

template <typename T> int transform(T value) {
    return static_cast<int>(value);
}

template int transform<int>(int);
template int transform<double>(double);

} // namespace templates

int initialize_global() {
    return 42;
}

int unused_initializer_like() {
    return 0;
}

int initialized_value = initialize_global();

#define DEFINE_FUNCTION(name)                                                                      \
    void name() {}
DEFINE_FUNCTION(macro_live)
DEFINE_FUNCTION(macro_dead)

auto unused_lambda = [](int value) { return value + 1; };

int main() {
    overloads::run();
    alpha::same_name();
    cross_tu_live();
    generated_live();
    internal_live();
    anonymous_live();
    invoke_callback_directly();
    register_callback(&escaped_callback);
    LiveAggregate aggregate;
    LiveBase* base = &aggregate;
    base->render();
    static_cast<void>(templates::transform(1));
    macro_live();
    return initialized_value;
}
