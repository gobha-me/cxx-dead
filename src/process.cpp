#include "cxx_dead/process.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
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

void append_available(int descriptor, std::string& destination, bool& open) {
    std::array<char, 16U * 1024U> buffer{};
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
        destination.append(buffer.data(), static_cast<std::size_t>(count));
    } else if (count == 0) {
        close_descriptor(descriptor);
        open = false;
    } else if (errno != EINTR && errno != EAGAIN) {
        close_descriptor(descriptor);
        open = false;
    }
}

} // namespace

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory) {
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

    close_descriptor(stdout_pipe[1]);
    close_descriptor(stderr_pipe[1]);
    ProcessResult result;
    bool stdout_open = true;
    bool stderr_open = true;
    while (stdout_open || stderr_open) {
        pollfd descriptors[2]{
            {stdout_pipe[0], static_cast<short>(stdout_open ? POLLIN : 0), 0},
            {stderr_pipe[0], static_cast<short>(stderr_open ? POLLIN : 0), 0},
        };
        const auto poll_result = ::poll(descriptors, 2, -1);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (stdout_open && (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            append_available(stdout_pipe[0], result.standard_output, stdout_open);
        }
        if (stderr_open && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            append_available(stderr_pipe[0], result.standard_error, stderr_open);
        }
        if (stdout_open && (descriptors[0].revents & POLLNVAL) != 0)
            stdout_open = false;
        if (stderr_open && (descriptors[1].revents & POLLNVAL) != 0)
            stderr_open = false;
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        throw std::runtime_error("cannot wait for child process: " +
                                 std::string(std::strerror(errno)));
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);
    return result;
}

} // namespace cxx_dead
