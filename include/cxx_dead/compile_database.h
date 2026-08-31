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
    std::filesystem::path output;
    std::vector<std::filesystem::path> command_inputs;
};

[[nodiscard]] std::vector<std::string> split_shell_command(std::string_view command);
[[nodiscard]] std::vector<CompileCommand>
load_compilation_database(const std::filesystem::path& path);
[[nodiscard]] CompileCommand normalize_compile_command(const CompileCommand& command);

} // namespace cxx_dead
