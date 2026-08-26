#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cxx_dead {

enum class ProcessTermination {
    Exited,
    Signaled,
    TimedOut,
    Cancelled,
    OutputLimitExceeded,
};

struct ProcessOptions {
    std::optional<std::chrono::milliseconds> timeout;
    std::size_t standard_output_limit{0};
    std::size_t standard_error_limit{64U * 1024U};
    std::function<bool()> cancellation_requested;
    std::chrono::milliseconds termination_grace{250};
};

struct ProcessResult {
    int exit_code{-1};
    int signal{0};
    ProcessTermination termination{ProcessTermination::Exited};
    std::string standard_output;
    std::string standard_error;
};

[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& arguments,
                                        const std::filesystem::path& working_directory,
                                        const ProcessOptions& options = {});

} // namespace cxx_dead
