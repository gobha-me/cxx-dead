#include <functional>

namespace callable_fixture {

using Callback = void (*)();

void statically_called() {}
void escaped_function() {}
void registered_function() {}
void unreachable_registered() {}
void unused_function() {}
void reassigned_first() {}
void reassigned_second() {}

struct Handler {
    void member_callback() {}
};

void consume(std::function<void()>) {}
void registrar(Callback) {}
void member_registrar(void (Handler::*)()) {}

void unreachable_registration_site() {
    registrar(&unreachable_registered);
}

void run() {
    Callback direct = &statically_called;
    direct();

    Callback reassigned = &reassigned_first;
    reassigned = &reassigned_second;
    reassigned();

    std::function<void()> stored = &escaped_function;
    consume(stored);

    auto direct_lambda = [] {};
    direct_lambda();

    auto escaped_lambda = [] {};
    consume(escaped_lambda);

    registrar(&registered_function);
    member_registrar(&Handler::member_callback);
}

auto unused_lambda = [] {};

} // namespace callable_fixture
