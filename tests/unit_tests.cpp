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
    const auto root = graph.add_or_merge_symbol({.key = "root",
                                                 .name = "root",
                                                 .scope = cxx_dead::SymbolScope::Reportable,
                                                 .defined = true});
    const auto live = graph.add_or_merge_symbol(
        {.key = "live", .name = "live", .scope = cxx_dead::SymbolScope::Indexed, .defined = true});
    const auto dead_a = graph.add_or_merge_symbol(
        {.key = "a", .name = "a", .scope = cxx_dead::SymbolScope::Reportable, .defined = true});
    const auto dead_b = graph.add_or_merge_symbol(
        {.key = "b", .name = "b", .scope = cxx_dead::SymbolScope::Reportable, .defined = true});
    const auto escaped = graph.add_or_merge_symbol({.key = "escaped",
                                                    .name = "escaped",
                                                    .scope = cxx_dead::SymbolScope::Reportable,
                                                    .defined = true});
    const auto opaque = graph.add_or_merge_symbol(
        {.key = "opaque", .name = "opaque", .scope = cxx_dead::SymbolScope::ExternalOpaque});
    const auto behind_opaque =
        graph.add_or_merge_symbol({.key = "behind_opaque",
                                   .name = "behind_opaque",
                                   .scope = cxx_dead::SymbolScope::Reportable,
                                   .defined = true});
    const cxx_dead::Evidence test_evidence{.provider = "test_provider", .reason = "arbitrary"};
    graph.add_root(root, cxx_dead::RootKind::Manual, test_evidence);
    graph.add_root(root, cxx_dead::RootKind::Manual, test_evidence);
    graph.add_edge(root, live, cxx_dead::EdgeKind::DirectCall, test_evidence);
    graph.add_edge(root, live, cxx_dead::EdgeKind::DirectCall, test_evidence);
    graph.add_edge(dead_a, dead_b, cxx_dead::EdgeKind::DirectCall, test_evidence);
    graph.add_edge(dead_b, dead_a, cxx_dead::EdgeKind::DirectCall, test_evidence);
    graph.add_escape(escaped, cxx_dead::EscapeKind::AddressTaken, test_evidence, root);
    graph.add_escape(escaped, cxx_dead::EscapeKind::AddressTaken, test_evidence, root);
    graph.add_edge(root, opaque, cxx_dead::EdgeKind::DirectCall, test_evidence);
    graph.add_edge(opaque, behind_opaque, cxx_dead::EdgeKind::DirectCall, test_evidence);

    const auto promoted = graph.add_or_merge_symbol(
        {.key = "promoted", .scope = cxx_dead::SymbolScope::ExternalOpaque});
    require(graph.add_or_merge_symbol(
                {.key = "promoted", .scope = cxx_dead::SymbolScope::Indexed}) == promoted &&
                graph.add_or_merge_symbol(
                    {.key = "promoted", .scope = cxx_dead::SymbolScope::Reportable}) == promoted &&
                graph.symbols()[promoted].scope == cxx_dead::SymbolScope::Reportable,
            "merged symbol did not promote to its strongest scope");

    const auto sourced = graph.add_or_merge_symbol(
        {.key = "sourced", .scope = cxx_dead::SymbolScope::ExternalOpaque});
    require(
        graph.add_or_merge_symbol({
            .key = "sourced",
            .source = {.spelling =
                           {
                               .location = {.file = "definition.cpp", .line = 7, .column = 4},
                               .begin = {.file = "definition.cpp", .line = 7, .column = 1},
                               .end = {.file = "definition.cpp", .line = 9, .column = 2},
                           }},
            .scope = cxx_dead::SymbolScope::Indexed,
            .defined = true,
        }) == sourced &&
            graph.add_or_merge_symbol({
                .key = "sourced",
                .source = {.spelling =
                               {
                                   .location = {.file = "declaration.cpp", .line = 22, .column = 4},
                               }},
                .scope = cxx_dead::SymbolScope::Reportable,
            }) == sourced &&
            graph.symbols()[sourced].scope == cxx_dead::SymbolScope::Reportable &&
            cxx_dead::primary_source_extent(graph.symbols()[sourced]).location.line == 7U,
        "merged symbol did not retain its complete definition extent while promoting scope");

    const auto result = cxx_dead::analyze_reachability(graph);
    require(result.reachable[root] && result.reachable[live], "direct calls should be traversed");
    require(!result.reachable[escaped], "address escapes must not imply a call");
    require(result.reachable[opaque] && !result.reachable[behind_opaque],
            "external opaque symbol did not terminate traversal");
    require(!result.reachable[dead_a] && !result.reachable[dead_b],
            "dead cycle was marked reachable");
    const bool found_cycle = std::ranges::any_of(
        result.unreachable_sccs, [](const auto& component) { return component.size() == 2U; });
    require(found_cycle, "Tarjan analysis did not identify the dead cycle");
    require(graph.roots().size() == 1U && graph.edges().size() == 5U &&
                graph.escapes().size() == 1U,
            "duplicate structured evidence facts were not deduplicated");
    require(graph.edges().front().evidence == test_evidence,
            "graph edge did not retain its provider evidence");

    bool invalid_escape_rejected = false;
    try {
        graph.add_escape(graph.symbols().size(), cxx_dead::EscapeKind::AddressTaken, test_evidence);
    } catch (const std::out_of_range&) {
        invalid_escape_rejected = true;
    }
    require(invalid_escape_rejected, "escape accepted an invalid symbol reference");

    bool excluded_symbol_rejected = false;
    try {
        static_cast<void>(graph.add_or_merge_symbol(
            {.key = "excluded", .scope = cxx_dead::SymbolScope::Excluded}));
    } catch (const std::invalid_argument&) {
        excluded_symbol_rejected = true;
    }
    require(excluded_symbol_rejected, "excluded symbol entered the graph");

    const auto report = cxx_dead::build_report(graph, result);
    const auto escaped_finding =
        std::ranges::find_if(report.findings, [=](const cxx_dead::Finding& finding) {
            return finding.symbol == escaped;
        });
    require(escaped_finding != report.findings.end() &&
                escaped_finding->classification == cxx_dead::Classification::DynamicallyReferenced,
            "escape classification should depend on the typed fact, not its reason text");
    require(escaped_finding->evidence.size() == 2U && escaped_finding->evidence.back().from == root,
            "classification did not retain the specific escape evidence");
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
    const auto escaped_fact =
        std::ranges::find_if(indexed.graph.escapes(), [=](const cxx_dead::Escape& escape) {
            return escape.symbol == escaped;
        });
    require(!reachability.reachable[escaped] && escaped_fact != indexed.graph.escapes().end(),
            "escaped callback should retain an escape fact, not become statically called");
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
    require(report_json.find("schema_version") != nullptr &&
                report_json.find("schema_version")->as_number() == 4.0,
            "JSON report does not use source-mapping schema version 4");
    require(report_json.find("roots") != nullptr && report_json.find("roots")->is_array(),
            "JSON report has no structured roots field");

    std::ostringstream human_output;
    cxx_dead::write_human_report(human_output, indexed.graph, reachability, report,
                                 indexed.diagnostics);
    require(human_output.str().contains("ROOTS") &&
                human_output.str().contains("[escape] clang_ast"),
            "human report does not expose root and escape evidence");

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
    const auto configured =
        std::ranges::find_if(filtered.graph.roots(), [](const cxx_dead::Root& root) {
            return root.kind == cxx_dead::RootKind::Manual &&
                   root.evidence.provider == "command_line";
        });
    require(configured != filtered.graph.roots().end(),
            "configured root did not retain command-line evidence");
}

void test_libtooling_availability_contract() {
    if (cxx_dead::libtooling_available())
        return;
    try {
        static_cast<void>(cxx_dead::LibToolingIndexer({.project_root = "."}).index({}));
        throw std::runtime_error("unavailable LibTooling frontend unexpectedly ran");
    } catch (const std::exception& error) {
        require(std::string(error.what()).contains("CXX_DEAD_ENABLE_LIBTOOLING=ON"),
                "unavailable LibTooling frontend has no actionable diagnostic");
    }
}

} // namespace

int main() {
    try {
        test_json();
        test_shell_split();
        test_graph_algorithms();
        test_clang_integration();
        test_libtooling_availability_contract();
        std::cout << "all cxx-dead tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
