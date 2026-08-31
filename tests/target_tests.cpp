#include "cxx_dead/artifact.h"
#include "cxx_dead/build_model.h"
#include "cxx_dead/indexer.h"
#include "cxx_dead/json.h"
#include "cxx_dead/report.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

bool contains_target(const cxx_dead::TargetSelection& selection, std::string_view name) {
    return std::ranges::find(selection.context.closure_targets, name) !=
           selection.context.closure_targets.end();
}

bool has_finding(const cxx_dead::Graph& graph, const cxx_dead::AnalysisReport& report,
                 std::string_view qualified_name) {
    return std::ranges::any_of(report.findings, [&](const cxx_dead::Finding& finding) {
        return graph.symbols()[finding.symbol].qualified_name == qualified_name;
    });
}

std::vector<std::string> finding_names(const cxx_dead::Graph& graph,
                                       const cxx_dead::AnalysisReport& report) {
    std::vector<std::string> result;
    for (const auto& finding : report.findings)
        result.push_back(graph.symbols()[finding.symbol].qualified_name);
    std::ranges::sort(result);
    return result;
}

template <typename Action> void require_throws(Action action, std::string_view expected) {
    try {
        action();
    } catch (const std::exception& error) {
        require(std::string_view(error.what()).find(expected) != std::string_view::npos,
                "failure did not contain the expected diagnostic");
        return;
    }
    throw std::runtime_error("expected operation to fail");
}

cxx_dead::AnalysisReport analyze(const cxx_dead::TargetSelection& selection,
                                 const std::filesystem::path& source_root, cxx_dead::Graph& graph) {
    auto indexed = cxx_dead::ClangAstIndexer({.project_root = source_root,
                                              .configuration_id = selection.context.configuration})
                       .index(selection.commands);
    graph = std::move(indexed.graph);
    return cxx_dead::build_report(graph, cxx_dead::analyze_reachability(graph));
}

} // namespace

int main() {
    try {
        const std::filesystem::path source_root{CXX_DEAD_TARGET_FIXTURE_SOURCE};
        const std::filesystem::path build_root{CXX_DEAD_TARGET_FIXTURE_BUILD};
        const auto commands =
            cxx_dead::load_compilation_database(build_root / "compile_commands.json");
        require(std::ranges::all_of(commands,
                                    [](const cxx_dead::CompileCommand& command) {
                                        return !command.output.empty();
                                    }),
                "CMake compilation-database object outputs were not retained");

        const auto cmake_model = cxx_dead::load_cmake_file_api(build_root);
        require(cmake_model.configurations.size() == 1U, "expected one CMake configuration");
        require(cmake_model.source_root == source_root, "CMake source root was not preserved");

        const auto production =
            cxx_dead::select_target_commands(cmake_model, "Debug", "production_app", commands);
        const auto tests =
            cxx_dead::select_target_commands(cmake_model, "Debug", "test_app", commands);
        require(contains_target(production, "production_app") &&
                    contains_target(production, "core") && contains_target(production, "objects") &&
                    contains_target(production, "shared"),
                "production closure omitted a linked target");
        require(!contains_target(production, "test_app"),
                "production closure included the sibling test target");
        require(contains_target(tests, "test_app") && contains_target(tests, "core") &&
                    contains_target(tests, "objects"),
                "test closure omitted a linked target");
        require(!contains_target(tests, "production_app") && !contains_target(tests, "shared"),
                "test closure included a production-only target");
        require_throws(
            [&] {
                static_cast<void>(
                    cxx_dead::select_target_commands(cmake_model, "Debug", "", commands));
            },
            "multiple executable targets");
        require_throws(
            [&] {
                static_cast<void>(
                    cxx_dead::select_target_commands(cmake_model, "Release", "test_app", commands));
            },
            "configuration not found");

        auto ambiguous_commands = commands;
        const auto core_command = std::ranges::find_if(ambiguous_commands, [](const auto& command) {
            return command.file.filename() == "core.cpp";
        });
        require(core_command != ambiguous_commands.end(), "fixture has no core command");
        core_command->output.clear();
        auto duplicate = *core_command;
        duplicate.arguments.push_back("-DCXX_DEAD_AMBIGUOUS=1");
        ambiguous_commands.push_back(std::move(duplicate));
        require_throws(
            [&] {
                static_cast<void>(cxx_dead::select_target_commands(
                    cmake_model, "Debug", "production_app", ambiguous_commands));
            },
            "ambiguous compilation commands");

        auto multiconfig_commands = commands;
        const auto multiconfig_core =
            std::ranges::find_if(multiconfig_commands, [](const auto& command) {
                return command.file.filename() == "core.cpp";
            });
        require(multiconfig_core != multiconfig_commands.end(), "fixture has no core command");
        const auto marker = std::string("/CMakeFiles/core.dir/");
        const auto position = multiconfig_core->output.generic_string().find(marker);
        require(position != std::string::npos, "fixture core output has no target marker");
        auto debug_output = multiconfig_core->output.generic_string();
        debug_output.insert(position + marker.size(), "Debug/");
        multiconfig_core->output = debug_output;
        auto release_command = *multiconfig_core;
        auto release_output = release_command.output.generic_string();
        release_output.replace(position + marker.size(), std::string("Debug/").size(), "Release/");
        release_command.output = release_output;
        release_command.arguments.push_back("-DNDEBUG");
        multiconfig_commands.push_back(std::move(release_command));
        const auto multiconfig_selection = cxx_dead::select_target_commands(
            cmake_model, "Debug", "production_app", multiconfig_commands);
        require(std::ranges::any_of(multiconfig_selection.commands,
                                    [](const auto& command) {
                                        return command.file.filename() == "core.cpp" &&
                                               command.output.generic_string().find("/Debug/") !=
                                                   std::string::npos;
                                    }),
                "selected configuration did not disambiguate compile commands");

        cxx_dead::Graph production_graph;
        const auto production_report = analyze(production, source_root, production_graph);
        require(has_finding(production_graph, production_report, "core::testing_hook"),
                "test-only hook was not target-relative dead in production");
        require(!has_finding(production_graph, production_report, "core::production_api"),
                "production API was not reachable from the production executable");
        require(has_finding(production_graph, production_report, "core::test_initializer_only"),
                "unselected test-target initializer affected production reachability");

        if (cxx_dead::libtooling_available()) {
            const cxx_dead::IndexOptions options{
                .project_root = source_root,
                .configuration_id = production.context.configuration,
            };
            const auto tooling = cxx_dead::LibToolingIndexer(options).index(production.commands);
            const auto tooling_report = cxx_dead::build_report(
                tooling.graph, cxx_dead::analyze_reachability(tooling.graph));
            require(finding_names(tooling.graph, tooling_report) ==
                        finding_names(production_graph, production_report),
                    "target-aware LibTooling findings differ from AST JSON");
        }

        const cxx_dead::AnalysisMetadata metadata{
            .mode = "target",
            .configuration_id = "Debug",
            .configuration = production.context.configuration,
            .target_id = production.context.target_id,
            .target_name = production.context.target_name,
            .target_kind = std::string(cxx_dead::to_string(production.context.target_kind)),
            .closure_targets = production.context.closure_targets,
        };
        std::ostringstream report_output;
        cxx_dead::write_json_report(report_output, production_graph,
                                    cxx_dead::analyze_reachability(production_graph),
                                    production_report, production.diagnostics, metadata);
        const auto report_json = cxx_dead::json::parse(report_output.str());
        const auto* context = report_json.find("analysis_context");
        require(report_json.find("schema_version")->as_number() == 12.0 && context != nullptr &&
                    context->string_or("target_name") == "production_app" &&
                    context->string_or("configuration_id") == "Debug",
                "target report omitted versioned analysis context");

        std::ostringstream artifact_output;
        cxx_dead::write_graph_artifact(
            artifact_output, production_graph,
            {.configuration_id = "Debug",
             .configuration = production.context.configuration,
             .target_id = production.context.target_id,
             .target_name = production.context.target_name,
             .target_kind = std::string(cxx_dead::to_string(production.context.target_kind)),
             .closure_targets = production.context.closure_targets,
             .translation_units = production.commands.size()},
            production.diagnostics);
        const auto artifact_json = cxx_dead::json::parse(artifact_output.str());
        require(artifact_json.find("artifact_schema_version")->as_number() == 6.0 &&
                    artifact_json.find("analysis_context") != nullptr,
                "graph artifact omitted target analysis context");

        cxx_dead::Graph test_graph;
        const auto test_report = analyze(tests, source_root, test_graph);
        require(has_finding(test_graph, test_report, "core::production_api"),
                "production-only API was not target-relative dead in tests");
        require(!has_finding(test_graph, test_report, "core::testing_hook"),
                "test hook was not reachable from the test executable");
        require(!has_finding(test_graph, test_report, "core::test_initializer_only"),
                "selected test-target initializer did not retain its target");

        const auto manifest_model =
            cxx_dead::load_target_manifest(build_root / "target-manifest.json");
        const auto manifest_selection =
            cxx_dead::select_target_commands(manifest_model, "Debug", "production_app", commands);
        require(manifest_selection.commands.size() == production.commands.size(),
                "manifest fallback selected a different production closure");
        require(manifest_selection.context.closure_targets == production.context.closure_targets,
                "manifest fallback produced a different target closure");

        const auto shared =
            cxx_dead::select_target_commands(cmake_model, "Debug", "shared", commands);
        require(shared.context.public_headers.size() == 1U &&
                    shared.context.public_headers.front().filename() == "shared.hpp",
                "CMake public header file set was not retained");
        const cxx_dead::IndexOptions shared_options{
            .project_root = source_root,
            .configuration_id = shared.context.configuration,
            .selected_target_sources = shared.context.selected_sources,
            .public_headers = shared.context.public_headers,
            .infer_shared_library_exports = true,
        };
        const auto shared_indexed =
            cxx_dead::ClangAstIndexer(shared_options).index(shared.commands);
        const auto shared_reachability = cxx_dead::analyze_reachability(shared_indexed.graph);
        const auto shared_report =
            cxx_dead::build_report(shared_indexed.graph, shared_reachability);
        require(
            shared_report.public_api_symbols == 3U &&
                !has_finding(shared_indexed.graph, shared_report, "shared::unused_shared_api") &&
                !has_finding(shared_indexed.graph, shared_report, "shared::export_only_api") &&
                has_finding(shared_indexed.graph, shared_report, "shared::hidden_implementation") &&
                has_finding(shared_indexed.graph, shared_report, "shared::private_header_helper"),
            "library public API policy did not separate exported API from private code");
        if (cxx_dead::libtooling_available()) {
            const auto shared_tooling =
                cxx_dead::LibToolingIndexer(shared_options).index(shared.commands);
            const auto shared_tooling_report = cxx_dead::build_report(
                shared_tooling.graph, cxx_dead::analyze_reachability(shared_tooling.graph));
            require(finding_names(shared_tooling.graph, shared_tooling_report) ==
                            finding_names(shared_indexed.graph, shared_report) &&
                        shared_tooling_report.public_api_symbols ==
                            shared_report.public_api_symbols,
                    "library public API policy differs between AST JSON and LibTooling");
        }

        const auto core = cxx_dead::select_target_commands(cmake_model, "Debug", "core", commands);
        require_throws(
            [&] {
                static_cast<void>(
                    cxx_dead::ClangAstIndexer({.project_root = source_root,
                                               .configuration_id = core.context.configuration,
                                               .require_library_api_policy = true})
                        .index(core.commands));
            },
            "requires public headers or explicit public API roots");

        const auto manifest_shared =
            cxx_dead::select_target_commands(manifest_model, "Debug", "shared", commands);
        require(manifest_shared.context.public_headers == shared.context.public_headers,
                "manifest v2 public headers differ from CMake metadata");

        const auto header_only =
            cxx_dead::select_target_commands(manifest_model, "Debug", "header_only", commands);
        require(header_only.commands.empty() && header_only.context.public_headers.size() == 1U,
                "interface target did not preserve its observed-only analysis context");
        require_throws(
            [&] {
                static_cast<void>(cxx_dead::ClangAstIndexer(
                                      {.project_root = source_root,
                                       .configuration_id = header_only.context.configuration,
                                       .public_headers = header_only.context.public_headers,
                                       .require_library_api_policy = true})
                                      .index(header_only.commands));
            },
            "has no compilation context");
        require_throws(
            [&] {
                static_cast<void>(cxx_dead::ClangAstIndexer(
                                      {.project_root = source_root,
                                       .configuration_id = shared.context.configuration,
                                       .selected_target_sources = shared.context.selected_sources,
                                       .public_headers = {source_root / "header_only.hpp"},
                                       .infer_shared_library_exports = true})
                                      .index(shared.commands));
            },
            "public header was not observed");

        std::cout << "target-aware tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "target test failure: " << error.what() << '\n';
        return 1;
    }
}
