#include <memory>
#include <optional>

namespace construction_fixture {

struct DirectProduct {
    explicit DirectProduct(int) {}
    explicit DirectProduct(double) {}
    ~DirectProduct() {}
};

struct Base {
    Base() {}
    ~Base() {}
};

struct Member {
    Member() {}
    ~Member() {}
};

struct FactoryProduct : Base {
    explicit FactoryProduct(int) {}
    explicit FactoryProduct(double) {}
    ~FactoryProduct() {}

    Member member;
};

struct NonFactoryProduct {
    explicit NonFactoryProduct(int) {}
    ~NonFactoryProduct() {}
};

std::unique_ptr<FactoryProduct> custom_factory();
std::unique_ptr<NonFactoryProduct>& borrowed_product();
std::optional<std::unique_ptr<NonFactoryProduct>> nested_product();

int run() {
    DirectProduct direct(1);
    using DirectAlias = DirectProduct;
    DirectAlias alias(2);
    auto unique = std::make_unique<FactoryProduct>(3);
    auto shared = std::make_shared<FactoryProduct>(4);
    auto custom = custom_factory();
    auto& borrowed = borrowed_product();
    auto nested = nested_product();
    auto moved = std::move(custom);
    return unique != nullptr && shared != nullptr && moved != nullptr && borrowed != nullptr &&
           nested.has_value();
}

} // namespace construction_fixture
