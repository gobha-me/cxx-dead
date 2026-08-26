namespace core {
int production_api();
}
namespace objects {
int object_api();
}
namespace shared {
int shared_api();
}

int main() {
    return core::production_api() + objects::object_api() + shared::shared_api();
}
