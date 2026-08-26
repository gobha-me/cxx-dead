#include "cxx_dead/process.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cxx_dead {

namespace {

void close_descriptor(int descriptor) {
    if (descriptor >= 0)
        static_cast<void>(::close(descriptor));
}

bool append_available(int descriptor, std::string& destination, bool& open, std::size_t limit) {
    std::array<char, 16U * 1024U> buffer{};
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
        const auto available = static_cast<std::size_t>(count);
        if (limit != 0 && (destination.size() >= limit || available > limit - destination.size())) {
            const auto remaining = limit > destination.size() ? limit - destination.size() : 0U;
            destination.append(buffer.data(), remaining);
            return true;
        }
        destination.append(buffer.data(), available);
    } else if (count == 0) {
        close_descriptor(descriptor);
        open = false;
    } else if (errno != EINTR && errno != EAGAIN) {
        close_descriptor(descriptor);
        open = false;
    }
    return false;
}

void signal_process_group(pid_t child, int signal) {
    if (::kill(-child, signal) != 0 && errno == ESRCH)
        static_cast<void>(::kill(child, signal));
}

} // namespace

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory,
                          const ProcessOptions& options) {
    if (arguments.empty())
        throw std::invalid_argument("cannot run an empty command");

    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        close_descriptor(stdout_pipe[0]);
        close_descriptor(stdout_pipe[1]);
        close_descriptor(stderr_pipe[0]);
        close_descriptor(stderr_pipe[1]);
        throw std::runtime_error("cannot create process pipes: " +
                                 std::string(std::strerror(errno)));
    }

    const auto child = ::fork();
    if (child < 0) {
        close_descriptor(stdout_pipe[0]);
        close_descriptor(stdout_pipe[1]);
        close_descriptor(stderr_pipe[0]);
        close_descriptor(stderr_pipe[1]);
        throw std::runtime_error("cannot fork: " + std::string(std::strerror(errno)));
    }

    if (child == 0) {
        static_cast<void>(::setpgid(0, 0));
        static_cast<void>(::dup2(stdout_pipe[1], STDOUT_FILENO));
        static_cast<void>(::dup2(stderr_pipe[1], STDERR_FILENO));
        close_descriptor(stdout_pipe[0]);
        close_descriptor(stdout_pipe[1]);
        close_descriptor(stderr_pipe[0]);
        close_descriptor(stderr_pipe[1]);
        if (::chdir(working_directory.c_str()) != 0)
            _exit(126);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1U);
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    if (::setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        const auto group_error = errno;
        signal_process_group(child, SIGKILL);
        static_cast<void>(::waitpid(child, nullptr, 0));
        close_descriptor(stdout_pipe[0]);
        close_descriptor(stdout_pipe[1]);
        close_descriptor(stderr_pipe[0]);
        close_descriptor(stderr_pipe[1]);
        throw std::runtime_error("cannot create child process group: " +
                                 std::string(std::strerror(group_error)));
    }

    close_descriptor(stdout_pipe[1]);
    close_descriptor(stderr_pipe[1]);
    ProcessResult result;
    bool stdout_open = true;
    bool stderr_open = true;
    bool child_reaped = false;
    bool termination_requested = false;
    bool kill_sent = false;
    std::optional<std::chrono::steady_clock::time_point> termination_started;
    const auto deadline = options.timeout.has_value()
                              ? std::optional{std::chrono::steady_clock::now() + *options.timeout}
                              : std::nullopt;
    std::string poll_error;

    const auto request_termination = [&](ProcessTermination termination) {
        if (termination_requested)
            return;
        result.termination = termination;
        termination_requested = true;
        termination_started = std::chrono::steady_clock::now();
        signal_process_group(child, SIGTERM);
    };

    while (!child_reaped || stdout_open || stderr_open) {
        const auto now = std::chrono::steady_clock::now();
        if (!termination_requested && options.cancellation_requested &&
            options.cancellation_requested()) {
            request_termination(ProcessTermination::Cancelled);
        }
        if (!termination_requested && deadline.has_value() && now >= *deadline)
            request_termination(ProcessTermination::TimedOut);
        if (termination_requested && !kill_sent && termination_started.has_value() &&
            now - *termination_started >= options.termination_grace) {
            signal_process_group(child, SIGKILL);
            kill_sent = true;
        }

        pollfd descriptors[2]{
            {stdout_open ? stdout_pipe[0] : -1, POLLIN, 0},
            {stderr_open ? stderr_pipe[0] : -1, POLLIN, 0},
        };
        int poll_timeout = 50;
        if (!termination_requested && deadline.has_value()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       *deadline - std::chrono::steady_clock::now())
                                       .count();
            poll_timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, poll_timeout));
        }
        const auto poll_result = ::poll(descriptors, 2, poll_timeout);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            poll_error = "cannot poll child process: " + std::string(std::strerror(errno));
            request_termination(ProcessTermination::Cancelled);
        }
        if (stdout_open && (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            if (append_available(stdout_pipe[0], result.standard_output, stdout_open,
                                 options.standard_output_limit)) {
                request_termination(ProcessTermination::OutputLimitExceeded);
            }
        }
        if (stderr_open && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            static_cast<void>(append_available(stderr_pipe[0], result.standard_error, stderr_open,
                                               options.standard_error_limit));
        }
        if (stdout_open && (descriptors[0].revents & POLLNVAL) != 0)
            stdout_open = false;
        if (stderr_open && (descriptors[1].revents & POLLNVAL) != 0)
            stderr_open = false;

        if (!child_reaped) {
            int status = 0;
            const auto waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child) {
                child_reaped = true;
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.signal = WTERMSIG(status);
                    result.exit_code = 128 + result.signal;
                    if (!termination_requested)
                        result.termination = ProcessTermination::Signaled;
                }
            } else if (waited < 0 && errno != EINTR) {
                poll_error = "cannot wait for child process: " + std::string(std::strerror(errno));
                request_termination(ProcessTermination::Cancelled);
                child_reaped = true;
            }
        }
    }

    close_descriptor(stdout_pipe[0]);
    close_descriptor(stderr_pipe[0]);
    if (!poll_error.empty())
        throw std::runtime_error(poll_error);
    return result;
}

} // namespace cxx_dead
