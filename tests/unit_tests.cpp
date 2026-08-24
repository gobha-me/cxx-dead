#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"
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

cxx_dead::SymbolId find_symbol(const cxx_dead::Graph& graph, std::string_view qualified_name) {
    for (cxx_dead::SymbolId id = 0; id < graph.symbols().size(); ++id) {
        if (graph.symbols()[id].qualified_name == qualified_name && graph.symbols()[id].defined) {
            return id;
        }
    }
    throw std::runtime_error("missing symbol: " + std::string(qualified_name));
}

void test_json() {
    const auto value =
        cxx_dead::json::parse(R"({"name":"cxx-dead","items":[1,true,null],"unicode":"\u03bb"})");
    require(value.string_or("name") == "cxx-dead", "JSON string field was not parsed");
    require(value.find("items") != nullptr && value.find("items")->as_array().size() == 3U,
            "JSON array was not parsed");
    require(value.string_or("unicode") == "λ", "JSON unicode escape was not parsed");
}

void test_shell_split() {
    const auto arguments = cxx_dead::split_shell_command(
        R"(clang++ -I"path with spaces" -DNAME='two words' source.cpp)");
    require(arguments.size() == 4U, "shell command split produced the wrong argument count");
    require(arguments[1] == "-Ipath with spaces", "double quoted argument was not preserved");
    require(arguments[2] == "-DNAME=two words", "single quoted argument was not preserved");
}

void test_graph_algorithms() {
    cxx_dead::Graph graph;
    const auto root = graph.add_or_merge_symbol(
        {.key = "root", .name = "root", .defined = true, .project_owned = true});
    const auto live = graph.add_or_merge_symbol(
        {.key = "live", .name = "live", .defined = true, .project_owned = true});
    const auto dead_a = graph.add_or_merge_symbol(
        {.key = "a", .name = "a", .defined = true, .project_owned = true});
    const auto dead_b = graph.add_or_merge_symbol(
        {.key = "b", .name = "b", .defined = true, .project_owned = true});
    const auto escaped = graph.add_or_merge_symbol(
        {.key = "escaped", .name = "escaped", .defined = true, .project_owned = true});
    graph.add_root(root, "test");
    graph.add_edge(root, live, cxx_dead::EdgeKind::DirectCall);
    graph.add_edge(dead_a, dead_b, cxx_dead::EdgeKind::DirectCall);
    graph.add_edge(dead_b, dead_a, cxx_dead::EdgeKind::DirectCall);
    graph.add_edge(root, escaped, cxx_dead::EdgeKind::AddressTaken);

    const auto result = cxx_dead::analyze_reachability(graph);
    require(result.reachable[root] && result.reachable[live], "direct calls should be traversed");
    require(!result.reachable[escaped], "address-taken edges must not imply a call");
    require(!result.reachable[dead_a] && !result.reachable[dead_b],
            "dead cycle was marked reachable");
    const bool found_cycle = std::ranges::any_of(
        result.unreachable_sccs, [](const auto& component) { return component.size() == 2U; });
    require(found_cycle, "Tarjan analysis did not identify the dead cycle");
}

void test_clang_integration() {
    const auto fixture = std::filesystem::path(CXX_DEAD_FIXTURE_DIR);
    std::vector<cxx_dead::CompileCommand> commands;
    for (const auto file : {fixture / "app.cpp", fixture / "helper.cpp"}) {
        commands.push_back({
            .directory = fixture,
            .file = file,
            .arguments = {"clang++", "-std=c++23", "-c", file.string(), "-o", "ignored.o"},
        });
    }
    const cxx_dead::ClangAstIndexer indexer({.project_root = fixture});
    const auto indexed = indexer.index(commands);
    const auto reachability = cxx_dead::analyze_reachability(indexed.graph);

    require(reachability.reachable[find_symbol(indexed.graph, "live::run")],
            "main did not reach live::run");
    require(reachability.reachable[find_symbol(indexed.graph, "live_helper")],
            "cross-translation-unit call was not resolved");
    require(!reachability.reachable[find_symbol(indexed.graph, "dead_a")],
            "dead_a should be unreachable");
    require(!reachability.reachable[find_symbol(indexed.graph, "dead_b")],
            "dead_b should be unreachable");
    const auto escaped = find_symbol(indexed.graph, "escaped_callback");
    require(!reachability.reachable[escaped] && indexed.graph.symbols()[escaped].address_taken,
            "escaped callback should be uncertain, not statically called");
    require(reachability.reachable[find_symbol(indexed.graph, "initialize_global")],
            "namespace-scope initializer call should be a root");
    require(reachability.reachable[find_symbol(indexed.graph, "ConcreteRenderer::render")],
            "virtual dispatch did not reach the concrete override");
    require(
        reachability.reachable[find_symbol(indexed.graph, "ConcreteRenderer::ConcreteRenderer")],
        "construction through a type alias did not reach the concrete constructor");
    require(!reachability.reachable[find_symbol(indexed.graph, "LegacyParser::parse")],
            "unused type member should be unreachable");
    require(!reachability.reachable[find_symbol(indexed.graph, "internal_dead")],
            "unused internal function should be unreachable");
    const auto shared_member_count =
        std::ranges::count_if(indexed.graph.symbols(), [](const cxx_dead::Symbol& symbol) {
            return symbol.qualified_name == "SharedApi::unused_static_member" && symbol.defined;
        });
    require(
        shared_member_count == 1,
        "static class member should have one external-linkage identity across translation units");

    const auto report = cxx_dead::build_report(indexed.graph, reachability);
    std::ostringstream json_output;
    cxx_dead::write_json_report(json_output, indexed.graph, reachability, report,
                                indexed.diagnostics);
    const auto report_json = cxx_dead::json::parse(json_output.str());
    require(report_json.find("findings") != nullptr, "JSON report has no findings field");

    const cxx_dead::ClangAstIndexer filtered_indexer({
        .project_root = fixture,
        .ast_filter = "live",
        .manual_roots = {"live::run"},
    });
    const auto filtered = filtered_indexer.index(commands);
    const auto filtered_reachability = cxx_dead::analyze_reachability(filtered.graph);
    require(filtered_reachability.reachable[find_symbol(filtered.graph, "live::run")],
            "configured root was not retained by filtered indexing");
    require(filtered_reachability.reachable[find_symbol(filtered.graph, "live_helper")],
            "filtered indexing lost a cross-translation-unit call");
}

} // namespace

int main() {
    try {
        test_json();
        test_shell_split();
        test_graph_algorithms();
        test_clang_integration();
        std::cout << "all cxx-dead tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
