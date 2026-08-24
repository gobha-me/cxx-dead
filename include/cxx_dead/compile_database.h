#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cxx_dead {

struct CompileCommand {
    std::filesystem::path directory;
    std::filesystem::path file;
    std::vector<std::string> arguments;
};

[[nodiscard]] std::vector<std::string> split_shell_command(std::string_view command);
[[nodiscard]] std::vector<CompileCommand>
load_compilation_database(const std::filesystem::path& path);

} // namespace cxx_dead
