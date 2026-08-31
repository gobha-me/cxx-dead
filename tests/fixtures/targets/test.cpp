namespace core {
int testing_hook();
int test_initializer_only();
} // namespace core

int test_initialized_value = core::test_initializer_only();

int main() {
    return core::testing_hook() + test_initialized_value;
}
