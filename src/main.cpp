#include "cxx_dead/artifact.h"
#include "cxx_dead/build_model.h"
#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"
#include "cxx_dead/indexer.h"
#include "cxx_dead/report.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>

namespace {

struct CliOptions {
    std::filesystem::path compilation_database;
    std::filesystem::path project_root = std::filesystem::current_path();
    std::vector<std::filesystem::path> report_paths;
    std::optional<std::filesystem::path> translation_unit_root;
    std::vector<std::filesystem::path> excluded_paths;
    std::optional<std::filesystem::path> output;
    std::optional<std::filesystem::path> graph_output;
    std::optional<std::filesystem::path> cmake_build_directory;
    std::optional<std::filesystem::path> target_manifest;
    std::string format{"human"};
    std::string configuration_id{"default"};
    std::string configuration;
    std::string target;
    std::string clang{"clang++"};
    cxx_dead::IndexFrontend frontend{cxx_dead::IndexFrontend::AstJson};
    std::string ast_filter;
    std::vector<std::string> roots;
    bool verbose{false};
    bool clang_explicit{false};
    bool configuration_id_explicit{false};
    bool project_root_explicit{false};
    bool fail_on_unreachable{false};
};

void usage(std::ostream& output) {
    output << R"(Usage: cxx-dead [compile_commands.json] [options]

Application-mode whole-program C++ reachability prototype.

Options:
  --compile-commands PATH   Compilation database (default: compile_commands.json)
  --project-root PATH       Index definitions below this workspace path (default: cwd)
  --report-path PATH        Report definitions below this path (repeatable; default: project root)
  --tu-root PATH            Analyze only translation units below this path
  --exclude-path PATH       Exclude declarations below this path (repeatable)
  --mode application       Analysis context; only application is supported in the MVP
  --configuration-id ID    Stable build-configuration identity (default: default)
  --cmake-build-dir PATH    Read target metadata from a CMake File API reply
  --target-manifest PATH    Read target metadata from an explicit JSON manifest
  --configuration NAME     Select a build-model configuration
  --target NAME            Select one target by name or id
  --root SYMBOL            Add a qualified or mangled symbol root (repeatable)
  --frontend NAME          Index frontend: ast-json or libtooling (default: ast-json)
  --clang PATH              Clang executable used for AST indexing (default: clang++)
  --ast-filter TEXT         Experimental frontend declaration-name filter
  --format human|json      Output format (default: human)
  --output PATH             Write the report to a file
  --graph-output PATH       Write deterministic graph artifact JSON to a file
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
            std::cout << "cxx-dead 0.7.0\n";
            std::exit(0);
        }
        if (argument == "--compile-commands") {
            options.compilation_database = require_value(index, count, arguments, argument);
        } else if (argument == "--project-root") {
            options.project_root = require_value(index, count, arguments, argument);
            options.project_root_explicit = true;
        } else if (argument == "--report-path") {
            options.report_paths.emplace_back(require_value(index, count, arguments, argument));
        } else if (argument == "--tu-root") {
            options.translation_unit_root = require_value(index, count, arguments, argument);
        } else if (argument == "--exclude-path") {
            options.excluded_paths.emplace_back(require_value(index, count, arguments, argument));
        } else if (argument == "--mode") {
            const auto mode = require_value(index, count, arguments, argument);
            if (mode != "application")
                throw std::runtime_error("only --mode application is supported");
        } else if (argument == "--configuration-id") {
            options.configuration_id = require_value(index, count, arguments, argument);
            options.configuration_id_explicit = true;
            if (options.configuration_id.empty())
                throw std::runtime_error("--configuration-id cannot be empty");
        } else if (argument == "--cmake-build-dir") {
            options.cmake_build_directory = require_value(index, count, arguments, argument);
        } else if (argument == "--target-manifest") {
            options.target_manifest = require_value(index, count, arguments, argument);
        } else if (argument == "--configuration") {
            options.configuration = require_value(index, count, arguments, argument);
        } else if (argument == "--target") {
            options.target = require_value(index, count, arguments, argument);
        } else if (argument == "--root") {
            options.roots.push_back(require_value(index, count, arguments, argument));
        } else if (argument == "--frontend") {
            const auto frontend = require_value(index, count, arguments, argument);
            if (frontend == "ast-json")
                options.frontend = cxx_dead::IndexFrontend::AstJson;
            else if (frontend == "libtooling")
                options.frontend = cxx_dead::IndexFrontend::LibTooling;
            else
                throw std::runtime_error("--frontend must be ast-json or libtooling");
        } else if (argument == "--clang") {
            options.clang = require_value(index, count, arguments, argument);
            options.clang_explicit = true;
        } else if (argument == "--ast-filter") {
            options.ast_filter = require_value(index, count, arguments, argument);
        } else if (argument == "--format") {
            options.format = require_value(index, count, arguments, argument);
            if (options.format != "human" && options.format != "json") {
                throw std::runtime_error("--format must be human or json");
            }
        } else if (argument == "--output") {
            options.output = require_value(index, count, arguments, argument);
        } else if (argument == "--graph-output") {
            options.graph_output = require_value(index, count, arguments, argument);
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
    if (options.cmake_build_directory.has_value() && options.target_manifest.has_value())
        throw std::runtime_error("--cmake-build-dir and --target-manifest are mutually exclusive");
    if ((!options.configuration.empty() || !options.target.empty()) &&
        !options.cmake_build_directory.has_value() && !options.target_manifest.has_value()) {
        throw std::runtime_error("--configuration and --target require build metadata");
    }
    if (options.translation_unit_root.has_value() &&
        (options.cmake_build_directory.has_value() || options.target_manifest.has_value())) {
        throw std::runtime_error("--tu-root cannot be combined with target-aware build metadata");
    }
    if (options.cmake_build_directory.has_value()) {
        options.cmake_build_directory =
            std::filesystem::absolute(*options.cmake_build_directory).lexically_normal();
    }
    if (options.target_manifest.has_value()) {
        options.target_manifest =
            std::filesystem::absolute(*options.target_manifest).lexically_normal();
    }
    if (options.compilation_database.empty()) {
        options.compilation_database =
            options.cmake_build_directory.has_value()
                ? *options.cmake_build_directory / "compile_commands.json"
                : std::filesystem::path{"compile_commands.json"};
    }
    options.compilation_database =
        std::filesystem::absolute(options.compilation_database).lexically_normal();
    options.project_root = std::filesystem::absolute(options.project_root).lexically_normal();
    if (options.output.has_value())
        options.output = std::filesystem::absolute(*options.output).lexically_normal();
    if (options.graph_output.has_value())
        options.graph_output = std::filesystem::absolute(*options.graph_output).lexically_normal();
    if (options.output.has_value() && options.graph_output == options.output)
        throw std::runtime_error("--output and --graph-output must name different files");
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
        auto options = parse_cli(argc, argv);
        if (options.frontend == cxx_dead::IndexFrontend::LibTooling && options.clang_explicit) {
            throw std::runtime_error("--clang applies only to --frontend ast-json");
        }
        auto commands = cxx_dead::load_compilation_database(options.compilation_database);
        std::optional<cxx_dead::TargetSelection> target_selection;
        if (options.cmake_build_directory.has_value() || options.target_manifest.has_value()) {
            const auto model = options.cmake_build_directory.has_value()
                                   ? cxx_dead::load_cmake_file_api(*options.cmake_build_directory)
                                   : cxx_dead::load_target_manifest(*options.target_manifest);
            target_selection = cxx_dead::select_target_commands(model, options.configuration,
                                                                options.target, commands);
            commands = target_selection->commands;
            if (!options.project_root_explicit)
                options.project_root = model.source_root;
            if (!options.configuration_id_explicit) {
                options.configuration_id = target_selection->context.configuration.empty()
                                               ? "default"
                                               : target_selection->context.configuration;
            }
        }
        if (options.translation_unit_root.has_value()) {
            std::erase_if(commands, [&](const cxx_dead::CompileCommand& command) {
                return !path_is_within(command.file, *options.translation_unit_root);
            });
            if (commands.empty())
                throw std::runtime_error("--tu-root excluded every compilation command");
        }
        const cxx_dead::IndexOptions index_options{
            .project_root = options.project_root,
            .configuration_id = options.configuration_id,
            .report_paths = options.report_paths,
            .excluded_paths = options.excluded_paths,
            .clang_executable = options.clang,
            .ast_filter = options.ast_filter,
            .manual_roots = options.roots,
            .verbose = options.verbose,
        };
        const auto started = std::chrono::steady_clock::now();
        auto indexed = options.frontend == cxx_dead::IndexFrontend::LibTooling
                           ? cxx_dead::LibToolingIndexer(index_options).index(commands)
                           : cxx_dead::ClangAstIndexer(index_options).index(commands);
        if (target_selection.has_value()) {
            indexed.diagnostics.insert(indexed.diagnostics.end(),
                                       target_selection->diagnostics.begin(),
                                       target_selection->diagnostics.end());
            std::ranges::sort(indexed.diagnostics);
            indexed.diagnostics.erase(std::ranges::unique(indexed.diagnostics).begin(),
                                      indexed.diagnostics.end());
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (options.verbose) {
            rusage self_usage{};
            rusage child_usage{};
            const auto self_peak =
                ::getrusage(RUSAGE_SELF, &self_usage) == 0 ? self_usage.ru_maxrss : 0;
            const auto child_peak =
                ::getrusage(RUSAGE_CHILDREN, &child_usage) == 0 ? child_usage.ru_maxrss : 0;
            const auto peak_rss = std::max(self_peak, child_peak);
            std::cerr << "cxx-dead-index-metrics"
                      << " frontend=" << cxx_dead::to_string(indexed.frontend)
                      << " translation_units=" << indexed.translation_units
                      << " ast_bytes=" << indexed.ast_bytes << " fact_bytes=" << indexed.fact_bytes
                      << " wall_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                      << " peak_rss_kib=" << peak_rss
                      << " symbols=" << indexed.graph.symbols().size()
                      << " edges=" << indexed.graph.edges().size() << '\n';
        }
        const auto reachability = cxx_dead::analyze_reachability(indexed.graph);
        const auto report = cxx_dead::build_report(indexed.graph, reachability);
        cxx_dead::AnalysisMetadata report_metadata{
            .mode = target_selection.has_value() ? "target" : "application",
            .configuration_id = options.configuration_id,
        };
        cxx_dead::GraphArtifactMetadata artifact_metadata{
            .configuration_id = options.configuration_id,
            .frontend = indexed.frontend,
            .translation_units = indexed.translation_units,
        };
        if (target_selection.has_value()) {
            const auto& context = target_selection->context;
            report_metadata.configuration = context.configuration;
            report_metadata.target_id = context.target_id;
            report_metadata.target_name = context.target_name;
            report_metadata.target_kind = cxx_dead::to_string(context.target_kind);
            report_metadata.closure_targets = context.closure_targets;
            artifact_metadata.configuration = context.configuration;
            artifact_metadata.target_id = context.target_id;
            artifact_metadata.target_name = context.target_name;
            artifact_metadata.target_kind = cxx_dead::to_string(context.target_kind);
            artifact_metadata.closure_targets = context.closure_targets;
        }

        if (options.graph_output.has_value()) {
            std::ofstream graph_output(*options.graph_output);
            if (!graph_output) {
                throw std::runtime_error("cannot open graph artifact: " +
                                         options.graph_output->string());
            }
            cxx_dead::write_graph_artifact(graph_output, indexed.graph, artifact_metadata,
                                           indexed.diagnostics);
        }

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
                                        indexed.diagnostics, report_metadata);
        } else {
            cxx_dead::write_human_report(*output, indexed.graph, reachability, report,
                                         indexed.diagnostics, report_metadata);
        }
        return options.fail_on_unreachable && !report.findings.empty() ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "cxx-dead: error: " << error.what() << '\n';
        return 1;
    }
}
