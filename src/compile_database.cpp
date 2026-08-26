#include "cxx_dead/compile_database.h"

#include "cxx_dead/json.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cxx_dead {

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

} // namespace cxx_dead
