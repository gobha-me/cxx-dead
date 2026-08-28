#include "cxx_dead/cache.h"
#include "cxx_dead/compile_database.h"
#include "cxx_dead/indexer.h"
#include "cxx_dead/provider.h"
#include "cxx_dead/report.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<std::string> edge_facts(const cxx_dead::Graph& graph) {
    std::vector<std::string> facts;
    for (const auto& edge : graph.edges()) {
        facts.push_back(graph.symbols()[edge.from].key + " -> " + graph.symbols()[edge.to].key +
                        " [" + std::string(cxx_dead::to_string(edge.kind)) + "] " +
                        edge.evidence.provider + ": " + edge.evidence.reason);
    }
    std::ranges::sort(facts);
    return facts;
}

std::string joined(const std::vector<std::string>& values) {
    std::ostringstream output;
    for (const auto& value : values)
        output << "\n  " << value;
    return output.str();
}

std::string report_json(const cxx_dead::IndexResult& indexed) {
    const auto reachability = cxx_dead::analyze_reachability(indexed.graph);
    const auto report = cxx_dead::build_report(indexed.graph, reachability);
    std::ostringstream output;
    cxx_dead::write_json_report(output, indexed.graph, reachability, report, {});
    return output.str();
}

void test_golden_frontend_parity() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const cxx_dead::IndexOptions options{.project_root = fixture};
    const auto ast = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto tooling = cxx_dead::LibToolingIndexer(options).index(commands);
    require(report_json(ast) == report_json(tooling),
            "LibTooling report differs from AST JSON report");
    const auto ast_edges = edge_facts(ast.graph);
    const auto tooling_edges = edge_facts(tooling.graph);
    require(ast_edges == tooling_edges,
            "LibTooling edges differ from AST JSON edges\nAST:" + joined(ast_edges) +
                "\nLibTooling:" + joined(tooling_edges));
    require(tooling.translation_units == ast.translation_units,
            "frontend translation-unit counts differ");
    require(tooling.ast_bytes == 0U && tooling.fact_bytes > 0U && ast.ast_bytes > 0U &&
                ast.fact_bytes > 0U,
            "frontend byte metrics are incomplete");
    require(std::ranges::any_of(tooling.graph.symbols(),
                                [](const cxx_dead::Symbol& symbol) {
                                    return symbol.identity.linkage_name.empty() &&
                                           !symbol.identity.usr.empty() &&
                                           symbol.identity.quality ==
                                               cxx_dead::IdentityQuality::Stable;
                                }),
            "LibTooling did not use a Clang USR for an unmangled declaration");
    const auto lambda =
        std::ranges::find_if(tooling.graph.symbols(), [](const cxx_dead::Symbol& symbol) {
            return symbol.defined && symbol.qualified_name.ends_with("operator()") &&
                   symbol.signature == "int (int) const";
        });
    require(lambda != tooling.graph.symbols().end() && !lambda->identity.translation_unit.empty(),
            "LibTooling lambda identity is not translation-unit scoped");
}

void test_filtered_frontend_parity() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const cxx_dead::IndexOptions options{
        .project_root = fixture,
        .ast_filter = "cross_tu",
        .manual_roots = {"cross_tu_live"},
    };
    const auto ast = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto tooling = cxx_dead::LibToolingIndexer(options).index(commands);
    require(report_json(ast) == report_json(tooling),
            "filtered LibTooling report differs from AST JSON report");
}

void test_scope_frontend_parity() {
    const auto fixture = std::filesystem::path(CXX_DEAD_SCOPE_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const cxx_dead::IndexOptions options{
        .project_root = fixture,
        .report_paths = {fixture / "app"},
    };
    const auto ast = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto tooling = cxx_dead::LibToolingIndexer(options).index(commands);
    require(report_json(ast) == report_json(tooling),
            "scope-separated LibTooling report differs from AST JSON report");
    require(edge_facts(ast.graph) == edge_facts(tooling.graph),
            "scope-separated LibTooling edges differ from AST JSON edges");
}

void test_construction_frontend_parity() {
    const auto fixture = std::filesystem::path(CXX_DEAD_CONSTRUCTION_FIXTURE_DIR);
    const auto source = fixture / "main.cpp";
    const std::vector<cxx_dead::CompileCommand> commands{{
        .directory = fixture,
        .file = source,
        .arguments = {"clang++", "-std=c++23", "-c", source.string(), "-o", "ignored.o"},
    }};
    const cxx_dead::IndexOptions options{
        .project_root = fixture,
        .ast_filter = "construction_fixture",
        .manual_roots = {"construction_fixture::run"},
    };
    const auto ast = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto tooling = cxx_dead::LibToolingIndexer(options).index(commands);
    require(report_json(ast) == report_json(tooling),
            "construction LibTooling report differs from AST JSON report");
    require(edge_facts(ast.graph) == edge_facts(tooling.graph),
            "construction LibTooling edges differ from AST JSON edges\nAST:" +
                joined(edge_facts(ast.graph)) +
                "\nLibTooling:" + joined(edge_facts(tooling.graph)));

    const auto unsupported_factory_diagnostics = [](const cxx_dead::IndexResult& indexed) {
        std::vector<std::string> result;
        std::ranges::copy_if(
            indexed.diagnostics, std::back_inserter(result), [](const std::string& diagnostic) {
                return diagnostic.starts_with("unsupported owning-pointer factory custom_factory");
            });
        return result;
    };
    require(unsupported_factory_diagnostics(ast) == unsupported_factory_diagnostics(tooling) &&
                unsupported_factory_diagnostics(ast).size() == 1U,
            "construction frontends produced different unsupported-factory diagnostics");
}

void test_callable_frontend_parity() {
    const auto fixture = std::filesystem::path(CXX_DEAD_CALLABLE_FIXTURE_DIR);
    const auto source = fixture / "main.cpp";
    const std::vector<cxx_dead::CompileCommand> commands{{
        .directory = fixture,
        .file = source,
        .arguments = {"clang++", "-std=c++23", "-c", source.string(), "-o", "ignored.o"},
    }};
    const cxx_dead::IndexOptions options{
        .project_root = fixture,
        .ast_filter = "callable_fixture",
        .manual_roots = {"callable_fixture::run"},
        .callback_registration_rules =
            {
                {.callee = cxx_dead::SymbolSelector{"callable_fixture::member_registrar"},
                 .argument_index = 0},
                {.callee = cxx_dead::SymbolSelector{"callable_fixture::registrar"},
                 .argument_index = 0},
            },
    };
    const auto ast = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto tooling = cxx_dead::LibToolingIndexer(options).index(commands);
    require(report_json(ast) == report_json(tooling),
            "callable LibTooling report differs from AST JSON report");
    require(edge_facts(ast.graph) == edge_facts(tooling.graph),
            "callable LibTooling edges differ from AST JSON edges\nAST:" +
                joined(edge_facts(ast.graph)) +
                "\nLibTooling:" + joined(edge_facts(tooling.graph)));
}

void test_provider_frontend_parity() {
    const auto fixture = std::filesystem::path(CXX_DEAD_PROVIDER_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const auto policy = cxx_dead::load_provider_config(fixture / "provider.yaml");
    const cxx_dead::IndexOptions options{
        .project_root = fixture,
        .ast_filter = "provider_fixture",
        .callback_registration_rules = policy.callback_registrations,
        .provider_policies = {policy},
    };
    auto ast = cxx_dead::ClangAstIndexer(options).index(commands);
    auto tooling = cxx_dead::LibToolingIndexer(options).index(commands);
    require(report_json(ast) == report_json(tooling),
            "provider LibTooling report differs from AST JSON report");
    require(edge_facts(ast.graph) == edge_facts(tooling.graph),
            "provider LibTooling edges differ from AST JSON edges\nAST:" +
                joined(edge_facts(ast.graph)) +
                "\nLibTooling:" + joined(edge_facts(tooling.graph)));
}

void test_exclusion_frontend_parity() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const cxx_dead::IndexOptions options{
        .project_root = fixture,
        .excluded_paths = {fixture / "generated"},
    };
    const auto ast = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto tooling = cxx_dead::LibToolingIndexer(options).index(commands);
    require(report_json(ast) == report_json(tooling),
            "excluded-path LibTooling report differs from AST JSON report");
}

void test_incomplete_libtooling_run_fails_closed() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto source = fixture / "invalid.cpp";
    const std::vector<cxx_dead::CompileCommand> commands{{
        .directory = fixture,
        .file = source,
        .arguments = {"clang++", "-std=c++23", "-c", source.string(), "-o", "invalid.o"},
    }};
    try {
        static_cast<void>(cxx_dead::LibToolingIndexer({.project_root = fixture}).index(commands));
        throw std::runtime_error("invalid translation unit unexpectedly produced an index");
    } catch (const std::exception& error) {
        require(std::string(error.what()).find("Clang LibTooling indexing failed for") !=
                    std::string::npos,
                "incomplete LibTooling run did not identify the failed frontend");
    }
}

void test_libtooling_hard_limits_are_explicitly_unsupported() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    commands.resize(1U);
    try {
        static_cast<void>(
            cxx_dead::LibToolingIndexer({
                                            .project_root = fixture,
                                            .translation_unit_timeout = std::chrono::seconds(1),
                                        })
                .index(commands));
        throw std::runtime_error("LibTooling unexpectedly accepted a hard timeout");
    } catch (const cxx_dead::IndexingError& error) {
        require(error.diagnostics().state == cxx_dead::RunState::Unsupported &&
                    error.diagnostics().translation_units.front().status ==
                        cxx_dead::TranslationUnitStatus::Unsupported,
                "LibTooling hard limit did not produce an unsupported run diagnostic");
    }
}

void test_libtooling_incremental_cache() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto cache = cxx_dead::cache_temporary_path("-libtooling-cache-test");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{cache};
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const cxx_dead::IndexOptions options{
        .project_root = fixture,
        .configuration_id = "libtooling-cache-test",
        .cache_directory = cache,
    };
    const auto cold = cxx_dead::LibToolingIndexer(options).index(commands);
    const auto warm = cxx_dead::LibToolingIndexer(options).index(commands);
    require(cold.metrics.cache_hits == 0U && cold.metrics.cache_misses == commands.size(),
            "cold LibTooling cache run did not index every translation unit");
    require(warm.metrics.cache_hits == commands.size() && warm.metrics.cache_misses == 0U,
            "warm LibTooling cache run did not reuse every translation unit");
    require(report_json(cold) == report_json(warm),
            "LibTooling cache reuse changed deterministic report output");
}

} // namespace

int main() {
    try {
        require(cxx_dead::libtooling_available(), "LibTooling build reports unavailable");
        test_golden_frontend_parity();
        test_filtered_frontend_parity();
        test_scope_frontend_parity();
        test_construction_frontend_parity();
        test_callable_frontend_parity();
        test_provider_frontend_parity();
        test_exclusion_frontend_parity();
        test_incomplete_libtooling_run_fails_closed();
        test_libtooling_hard_limits_are_explicitly_unsupported();
        test_libtooling_incremental_cache();
        std::cout << "all frontend parity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frontend test failure: " << error.what() << '\n';
        return 1;
    }
}
