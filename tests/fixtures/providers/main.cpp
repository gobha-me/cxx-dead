namespace provider_fixture {

using Callback = void (*)();

void registrar(Callback) {}

void registered_callback() {}

void global_registered_callback() {}

int global_registration = (registrar(&global_registered_callback), 1);

void run_registration() {
    registrar(&registered_callback);
}

void plugin_entry() {}

void plugin_leaf() {}

void escaped_callback() {}

void suppressed_callback() {}

void ordinary_dead() {}

void overloaded() {}

void overloaded(int) {}

} // namespace provider_fixture

int main() {
    provider_fixture::run_registration();
}
