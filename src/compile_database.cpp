#include "cxx_dead/compile_database.h"

#include "cxx_dead/json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace cxx_dead {

namespace {

constexpr std::size_t max_response_depth = 16U;
constexpr std::size_t max_response_bytes = 16U * 1024U * 1024U;

std::string executable_name(std::string_view argument) {
    return std::filesystem::path(argument).filename().string();
}

bool is_launcher(std::string_view argument) {
    const auto name = executable_name(argument);
    return name == "ccache" || name == "sccache" || name == "distcc" || name == "icecc";
}

bool version_suffix(std::string_view suffix) {
    if (suffix.empty())
        return false;
    return std::ranges::all_of(suffix, [](const char character) {
        return std::isdigit(static_cast<unsigned char>(character)) != 0 || character == '.';
    });
}

bool is_compiler(std::string_view argument) {
    auto name = executable_name(argument);
    if (const auto separator = name.rfind('-');
        separator != std::string::npos &&
        version_suffix(std::string_view(name).substr(separator + 1U))) {
        name.resize(separator);
    }
    constexpr std::string_view drivers[]{"clang++", "clang", "g++", "gcc", "c++", "cc"};
    return std::ranges::any_of(drivers, [&](const std::string_view driver) {
        return name == driver || (name.size() > driver.size() && name.ends_with(driver) &&
                                  name[name.size() - driver.size() - 1U] == '-');
    });
}

std::filesystem::path normalized_path(std::string_view value,
                                      const std::filesystem::path& directory) {
    auto path = std::filesystem::path(value);
    if (path.is_relative())
        path = directory / path;
    return std::filesystem::absolute(path).lexically_normal();
}

struct ResponseExpansion {
    const std::filesystem::path& directory;
    std::size_t bytes{0};
    std::unordered_set<std::string> active;
    std::vector<std::filesystem::path> inputs;
};

void expand_response_argument(std::string_view argument, std::size_t depth,
                              ResponseExpansion& expansion, std::vector<std::string>& output) {
    if (!argument.starts_with('@') || argument.size() == 1U) {
        output.emplace_back(argument);
        return;
    }
    if (depth >= max_response_depth)
        throw std::runtime_error("response-file nesting exceeds 16 levels");

    const auto path = normalized_path(argument.substr(1U), expansion.directory);
    const auto key = path.generic_string();
    if (!expansion.active.insert(key).second)
        throw std::runtime_error("cyclic response file: " + path.string());
    struct ActiveGuard {
        std::unordered_set<std::string>& active;
        std::string key;
        ~ActiveGuard() {
            active.erase(key);
        }
    } guard{expansion.active, key};

    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open response file: " + path.string());
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error)
        throw std::runtime_error("cannot inspect response file: " + path.string());
    if (file_bytes > max_response_bytes - std::min(expansion.bytes, max_response_bytes))
        throw std::runtime_error("response-file contents exceed 16 MiB");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
        throw std::runtime_error("cannot read response file: " + path.string());
    const auto text = contents.str();
    if (text.size() > max_response_bytes - std::min(expansion.bytes, max_response_bytes))
        throw std::runtime_error("response-file contents exceed 16 MiB");
    expansion.bytes += text.size();
    expansion.inputs.push_back(path);

    std::vector<std::string> arguments;
    try {
        arguments = split_shell_command(text);
    } catch (const std::exception& error) {
        throw std::runtime_error("invalid response file " + path.string() + ": " + error.what());
    }
    for (const auto& nested : arguments)
        expand_response_argument(nested, depth + 1U, expansion, output);
}

bool is_source_argument(std::string_view argument, const CompileCommand& command) {
    if (argument.empty() || argument.front() == '-')
        return false;
    return normalized_path(argument, command.directory) == command.file;
}

std::optional<std::string_view> joined_value(std::string_view argument, std::string_view option) {
    if (argument.starts_with(option) && argument.size() > option.size() &&
        argument[option.size()] == '=') {
        return argument.substr(option.size() + 1U);
    }
    return std::nullopt;
}

void add_command_input(std::string_view value, std::string_view option,
                       const CompileCommand& command, std::vector<std::filesystem::path>& inputs) {
    if (option == "-fmodule-file") {
        if (const auto separator = value.find('='); separator != std::string_view::npos) {
            const auto module_name = value.substr(0, separator);
            if (!module_name.empty() && !module_name.contains('/') && !module_name.contains('\\'))
                value.remove_prefix(separator + 1U);
        }
    }
    if (value.empty())
        throw std::runtime_error(std::string(option) + " requires a non-empty path");
    const auto path = normalized_path(value, command.directory);
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(path, status_error) || status_error)
        throw std::runtime_error("command input for " + std::string(option) +
                                 " is not a readable regular file: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read command input for " + std::string(option) + ": " +
                                 path.string());
    inputs.push_back(path);
}

void collect_command_inputs(const std::vector<std::string>& arguments,
                            const CompileCommand& command,
                            std::vector<std::filesystem::path>& inputs) {
    constexpr std::string_view separate_options[]{"-include-pch", "-fmodule-file",
                                                  "-fmodule-map-file"};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        bool matched = false;
        for (const auto option : separate_options) {
            if (argument == option) {
                if (index + 1U >= arguments.size())
                    throw std::runtime_error(std::string(option) + " requires a path");
                add_command_input(arguments[++index], option, command, inputs);
                matched = true;
                break;
            }
            if (const auto value = joined_value(argument, option)) {
                add_command_input(*value, option, command, inputs);
                matched = true;
                break;
            }
        }
        if (matched)
            continue;
    }
}

} // namespace

std::vector<std::string> split_shell_command(std::string_view command) {
    enum class Quote { None, Single, Double };
    Quote quote = Quote::None;
    bool escaped = false;
    bool token_started = false;
    std::string token;
    std::vector<std::string> result;

    const auto flush = [&] {
        if (token_started) {
            result.push_back(token);
            token.clear();
            token_started = false;
        }
    };

    for (const char c : command) {
        if (escaped) {
            token.push_back(c);
            token_started = true;
            escaped = false;
            continue;
        }
        if (quote == Quote::Single) {
            if (c == '\'')
                quote = Quote::None;
            else
                token.push_back(c);
            token_started = true;
            continue;
        }
        if (quote == Quote::Double) {
            if (c == '"')
                quote = Quote::None;
            else if (c == '\\')
                escaped = true;
            else
                token.push_back(c);
            token_started = true;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            token_started = true;
        } else if (c == '\'') {
            quote = Quote::Single;
            token_started = true;
        } else if (c == '"') {
            quote = Quote::Double;
            token_started = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            token.push_back(c);
            token_started = true;
        }
    }
    if (escaped || quote != Quote::None) {
        throw std::runtime_error("unterminated quote or escape in compilation command");
    }
    flush();
    return result;
}

std::vector<CompileCommand> load_compilation_database(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open compilation database: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const auto document = json::parse(contents.str());
    if (!document.is_array()) {
        throw std::runtime_error("compilation database must contain a JSON array");
    }

    std::vector<CompileCommand> commands;
    commands.reserve(document.as_array().size());
    const auto database_directory = std::filesystem::absolute(path).parent_path();
    for (const auto& item : document.as_array()) {
        if (!item.is_object()) {
            throw std::runtime_error("each compilation database entry must be an object");
        }
        CompileCommand command;
        command.directory = item.string_or("directory");
        command.file = item.string_or("file");
        if (command.directory.empty() || command.file.empty()) {
            throw std::runtime_error("compilation database entry is missing directory or file");
        }
        if (command.directory.is_relative()) {
            command.directory = database_directory / command.directory;
        }
        command.directory = std::filesystem::weakly_canonical(command.directory);
        if (command.file.is_relative()) {
            command.file = command.directory / command.file;
        }
        command.file = std::filesystem::weakly_canonical(command.file);
        command.output = item.string_or("output");
        if (!command.output.empty()) {
            if (command.output.is_relative())
                command.output = command.directory / command.output;
            command.output = std::filesystem::absolute(command.output).lexically_normal();
        }

        if (const auto* arguments = item.find("arguments"); arguments != nullptr) {
            if (!arguments->is_array()) {
                throw std::runtime_error("compilation database 'arguments' must be an array");
            }
            for (const auto& argument : arguments->as_array()) {
                if (!argument.is_string()) {
                    throw std::runtime_error("compilation database argument must be a string");
                }
                command.arguments.push_back(argument.as_string());
            }
        } else if (const auto* shell_command = item.find("command");
                   shell_command != nullptr && shell_command->is_string()) {
            command.arguments = split_shell_command(shell_command->as_string());
        } else {
            throw std::runtime_error("compilation database entry needs 'arguments' or 'command'");
        }
        if (command.arguments.empty()) {
            throw std::runtime_error("empty compilation command for " + command.file.string());
        }
        commands.push_back(std::move(command));
    }
    return commands;
}

CompileCommand normalize_compile_command(const CompileCommand& command) {
    if (command.arguments.empty())
        throw std::runtime_error("empty compilation command for " + command.file.string());

    auto resolved = command;
    if (resolved.directory.is_relative())
        resolved.directory = std::filesystem::absolute(resolved.directory);
    resolved.directory = resolved.directory.lexically_normal();
    if (resolved.file.is_relative())
        resolved.file = resolved.directory / resolved.file;
    resolved.file = std::filesystem::absolute(resolved.file).lexically_normal();
    if (!resolved.output.empty()) {
        if (resolved.output.is_relative())
            resolved.output = resolved.directory / resolved.output;
        resolved.output = std::filesystem::absolute(resolved.output).lexically_normal();
    }

    std::size_t compiler_index = 0;
    while (compiler_index < resolved.arguments.size() &&
           is_launcher(resolved.arguments[compiler_index])) {
        ++compiler_index;
    }
    if (compiler_index >= resolved.arguments.size() ||
        !is_compiler(resolved.arguments[compiler_index])) {
        const auto unsupported = compiler_index < resolved.arguments.size()
                                     ? resolved.arguments[compiler_index]
                                     : resolved.arguments.front();
        throw std::runtime_error("unsupported compiler or launcher command '" + unsupported +
                                 "' for " + resolved.file.string());
    }

    ResponseExpansion expansion{
        .directory = resolved.directory,
        .bytes = 0,
        .active = {},
        .inputs = {},
    };
    std::vector<std::string> expanded;
    for (std::size_t index = compiler_index + 1U; index < resolved.arguments.size(); ++index)
        expand_response_argument(resolved.arguments[index], 0U, expansion, expanded);

    const std::set<std::string, std::less<>> flags_with_value{"-o",
                                                              "--output",
                                                              "-MF",
                                                              "-MT",
                                                              "-MQ",
                                                              "-MJ",
                                                              "--serialize-diagnostics",
                                                              "-dependency-file",
                                                              "-fmodule-output"};
    const std::set<std::string, std::less<>> removed_flags{
        "-c", "-S", "-E", "-MD", "-MMD", "-MP", "-MG", "-MM", "-M", "-emit-llvm"};
    constexpr std::string_view joined_output_options[]{"--output=",
                                                       "-MF",
                                                       "-MT",
                                                       "-MQ",
                                                       "-MJ",
                                                       "--serialize-diagnostics=",
                                                       "-dependency-file=",
                                                       "-fmodule-output="};

    CompileCommand normalized{
        .directory = resolved.directory,
        .file = resolved.file,
        .arguments = {resolved.arguments[compiler_index]},
        .output = resolved.output,
        .command_inputs = std::move(expansion.inputs),
    };
    for (std::size_t index = 0; index < expanded.size(); ++index) {
        const auto& argument = expanded[index];
        if (flags_with_value.contains(argument)) {
            if (index + 1U >= expanded.size())
                throw std::runtime_error("compilation flag " + argument + " requires a value");
            ++index;
            continue;
        }
        if (removed_flags.contains(argument) ||
            std::ranges::any_of(joined_output_options,
                                [&](const std::string_view prefix) {
                                    return argument.starts_with(prefix) &&
                                           argument.size() > prefix.size();
                                }) ||
            (argument.starts_with("-o") && argument.size() > 2U)) {
            continue;
        }
        if (argument == "-Xclang" && index + 1U < expanded.size() &&
            (expanded[index + 1U] == "-dependency-file" ||
             expanded[index + 1U] == "-serialize-diagnostic-file")) {
            index += 1U;
            if (index + 2U < expanded.size() && expanded[index + 1U] == "-Xclang")
                index += 2U;
            continue;
        }
        if (argument.starts_with("-Wp,-MD,") || argument.starts_with("-Wp,-MMD,"))
            continue;
        if (is_source_argument(argument, resolved))
            continue;
        normalized.arguments.push_back(argument);
    }
    collect_command_inputs(normalized.arguments, resolved, normalized.command_inputs);
    std::ranges::sort(normalized.command_inputs);
    normalized.command_inputs.erase(std::ranges::unique(normalized.command_inputs).begin(),
                                    normalized.command_inputs.end());
    normalized.arguments.push_back(resolved.file.string());
    return normalized;
}

} // namespace cxx_dead
