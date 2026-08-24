#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cxx_dead {

struct ProcessResult {
    int exit_code{-1};
    std::string standard_output;
    std::string standard_error;
};

[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& arguments,
                                        const std::filesystem::path& working_directory);

} // namespace cxx_dead
