#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "clang";
    if (mode == "output") {
        for (int index = 0; index < 4096; ++index)
            std::cout << "0123456789abcdef";
        return 0;
    }
    if (mode == "spawn") {
        const auto child = ::fork();
        if (child < 0)
            return 2;
        while (true)
            ::pause();
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    return 0;
}
