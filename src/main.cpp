#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"
#include "cxx_dead/indexer.h"
#include "cxx_dead/report.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct CliOptions {
    std::filesystem::path compilation_database;
    std::filesystem::path project_root = std::filesystem::current_path();
    std::optional<std::filesystem::path> translation_unit_root;
    std::vector<std::filesystem::path> excluded_paths;
    std::optional<std::filesystem::path> output;
    std::string format{"human"};
    std::string clang{"clang++"};
    std::string ast_filter;
    std::vector<std::string> roots;
    bool verbose{false};
    bool fail_on_unreachable{false};
};

void usage(std::ostream& output) {
    output << R"(Usage: cxx-dead [compile_commands.json] [options]

Application-mode whole-program C++ reachability prototype.

Options:
  --compile-commands PATH   Compilation database (default: compile_commands.json)
  --project-root PATH       Only report definitions below this path (default: cwd)
  --tu-root PATH            Analyze only translation units below this path
  --exclude-path PATH       Exclude declarations below this path (repeatable)
  --mode application       Analysis context; only application is supported in the MVP
  --root SYMBOL            Add a qualified or mangled symbol root (repeatable)
  --clang PATH              Clang executable used for AST indexing (default: clang++)
  --ast-filter TEXT         Experimental Clang declaration-name filter
  --format human|json      Output format (default: human)
  --output PATH             Write the report to a file
  --fail-on-unreachable     Exit with status 2 when candidates are found
  --verbose                 Include Clang indexing diagnostics
  --help                    Show this help
  --version                 Show the version
)";
}

std::string require_value(int& index, int count, char** arguments, std::string_view option) {
    if (++index >= count)
        throw std::runtime_error(std::string(option) + " requires a value");
    return arguments[index];
}

CliOptions parse_cli(int count, char** arguments) {
    CliOptions options;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--help") {
            usage(std::cout);
            std::exit(0);
        }
        if (argument == "--version") {
            std::cout << "cxx-dead 0.1.1\n";
            std::exit(0);
        }
        if (argument == "--compile-commands") {
            options.compilation_database = require_value(index, count, arguments, argument);
        } else if (argument == "--project-root") {
            options.project_root = require_value(index, count, arguments, argument);
        } else if (argument == "--tu-root") {
            options.translation_unit_root = require_value(index, count, arguments, argument);
        } else if (argument == "--exclude-path") {
            options.excluded_paths.emplace_back(require_value(index, count, arguments, argument));
        } else if (argument == "--mode") {
            const auto mode = require_value(index, count, arguments, argument);
            if (mode != "application")
                throw std::runtime_error("only --mode application is supported");
        } else if (argument == "--root") {
            options.roots.push_back(require_value(index, count, arguments, argument));
        } else if (argument == "--clang") {
            options.clang = require_value(index, count, arguments, argument);
        } else if (argument == "--ast-filter") {
            options.ast_filter = require_value(index, count, arguments, argument);
        } else if (argument == "--format") {
            options.format = require_value(index, count, arguments, argument);
            if (options.format != "human" && options.format != "json") {
                throw std::runtime_error("--format must be human or json");
            }
        } else if (argument == "--output") {
            options.output = require_value(index, count, arguments, argument);
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--fail-on-unreachable") {
            options.fail_on_unreachable = true;
        } else if (argument.starts_with('-')) {
            throw std::runtime_error("unknown option: " + std::string(argument));
        } else if (options.compilation_database.empty()) {
            options.compilation_database = argument;
        } else {
            throw std::runtime_error("unexpected positional argument: " + std::string(argument));
        }
    }
    if (options.compilation_database.empty())
        options.compilation_database = "compile_commands.json";
    options.compilation_database =
        std::filesystem::absolute(options.compilation_database).lexically_normal();
    options.project_root = std::filesystem::absolute(options.project_root).lexically_normal();
    if (options.translation_unit_root.has_value()) {
        options.translation_unit_root =
            std::filesystem::absolute(*options.translation_unit_root).lexically_normal();
    }
    return options;
}

bool path_is_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto relative = child.lexically_relative(parent);
    if (relative.empty())
        return child == parent;
    return !relative.is_absolute() && *relative.begin() != "..";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_cli(argc, argv);
        auto commands = cxx_dead::load_compilation_database(options.compilation_database);
        if (options.translation_unit_root.has_value()) {
            std::erase_if(commands, [&](const cxx_dead::CompileCommand& command) {
                return !path_is_within(command.file, *options.translation_unit_root);
            });
            if (commands.empty())
                throw std::runtime_error("--tu-root excluded every compilation command");
        }
        cxx_dead::ClangAstIndexer indexer({
            .project_root = options.project_root,
            .excluded_paths = options.excluded_paths,
            .clang_executable = options.clang,
            .ast_filter = options.ast_filter,
            .manual_roots = options.roots,
            .verbose = options.verbose,
        });
        const auto indexed = indexer.index(commands);
        const auto reachability = cxx_dead::analyze_reachability(indexed.graph);
        const auto report = cxx_dead::build_report(indexed.graph, reachability);

        std::ofstream file_output;
        std::ostream* output = &std::cout;
        if (options.output.has_value()) {
            file_output.open(*options.output);
            if (!file_output)
                throw std::runtime_error("cannot open output file: " + options.output->string());
            output = &file_output;
        }
        if (options.format == "json") {
            cxx_dead::write_json_report(*output, indexed.graph, reachability, report,
                                        indexed.diagnostics);
        } else {
            cxx_dead::write_human_report(*output, indexed.graph, reachability, report,
                                         indexed.diagnostics);
        }
        return options.fail_on_unreachable && !report.findings.empty() ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "cxx-dead: error: " << error.what() << '\n';
        return 1;
    }
}
