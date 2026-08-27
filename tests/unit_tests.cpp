#include "cxx_dead/artifact.h"
#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"
#include "cxx_dead/indexer.h"
#include "cxx_dead/json.h"
#include "cxx_dead/process.h"
#include "cxx_dead/provider.h"
#include "cxx_dead/report.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

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

cxx_dead::SymbolId find_symbol(const cxx_dead::Graph& graph, std::string_view qualified_name,
                               std::string_view signature) {
    for (cxx_dead::SymbolId id = 0; id < graph.symbols().size(); ++id) {
        const auto& symbol = graph.symbols()[id];
        if (symbol.qualified_name == qualified_name && symbol.signature == signature &&
            symbol.defined) {
            return id;
        }
    }
    throw std::runtime_error("missing symbol: " + std::string(qualified_name) + " " +
                             std::string(signature));
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
    const auto provider = graph.add_or_merge_symbol({.key = "provider",
                                                     .name = "provider",
                                                     .scope = cxx_dead::SymbolScope::Reportable,
                                                     .defined = true});
    const auto both = graph.add_or_merge_symbol({.key = "both",
                                                 .name = "both",
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
    graph.add_edge(root, provider, cxx_dead::EdgeKind::CallbackRegistration, test_evidence);
    graph.add_edge(dead_a, provider, cxx_dead::EdgeKind::CallbackRegistration, test_evidence);
    graph.add_edge(root, both, cxx_dead::EdgeKind::CallbackRegistration, test_evidence);
    graph.add_edge(root, both, cxx_dead::EdgeKind::DirectCall, test_evidence);
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
    require(result.reachable[provider] && result.provider_reachable[provider] &&
                !result.structurally_reachable[provider],
            "callback registration did not retain provider provenance");
    require(result.structurally_reachable[both] && !result.provider_reachable[both],
            "provider provenance overrode an available structural path");
    require(result.reachable[opaque] && !result.reachable[behind_opaque],
            "external opaque symbol did not terminate traversal");
    require(!result.reachable[dead_a] && !result.reachable[dead_b],
            "dead cycle was marked reachable");
    const bool found_cycle = std::ranges::any_of(
        result.unreachable_sccs, [](const auto& component) { return component.size() == 2U; });
    require(found_cycle, "Tarjan analysis did not identify the dead cycle");
    require(graph.roots().size() == 1U && graph.edges().size() == 9U &&
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
    require(report.provider_reachable.size() == 1U &&
                report.provider_reachable.front().from == root,
            "provider provenance included an unreachable registration site");
}

void test_stable_identity_contract() {
    const auto external =
        cxx_dead::make_symbol_identity("debug", "c:@F@run#", "_Z3runv", "", "src/run.cpp:12");
    const auto external_other_source =
        cxx_dead::make_symbol_identity("debug", "c:@F@run#", "_Z3runv", "", "include/run.hpp:4");
    const auto other_configuration =
        cxx_dead::make_symbol_identity("release", "c:@F@run#", "_Z3runv", "", "src/run.cpp:12");
    const auto internal_a =
        cxx_dead::make_symbol_identity("debug", "", "_ZL6helperv", "src/a.cpp", "src/a.cpp:20");
    const auto internal_b =
        cxx_dead::make_symbol_identity("debug", "", "_ZL6helperv", "src/b.cpp", "src/b.cpp:20");
    const auto fallback = cxx_dead::make_symbol_identity("debug", "", "", "src/a.cpp",
                                                         "anonymous|void ()|src/a.cpp:20");

    require(cxx_dead::stable_symbol_key(external) ==
                cxx_dead::stable_symbol_key(external_other_source),
            "external identity depends on a declaration source location");
    require(cxx_dead::stable_symbol_key(external) !=
                cxx_dead::stable_symbol_key(other_configuration),
            "configuration identity did not disambiguate an external symbol");
    require(cxx_dead::stable_symbol_key(internal_a) != cxx_dead::stable_symbol_key(internal_b),
            "translation-unit identity did not disambiguate internal linkage");
    require(fallback.quality == cxx_dead::IdentityQuality::Fallback,
            "unsupported identity did not retain fallback quality");
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
    const cxx_dead::AnalysisMetadata indexed_metadata{
        .run = {.state = cxx_dead::RunState::Complete,
                .frontend = indexed.frontend,
                .partial_graph_discarded = false,
                .translation_units = indexed.translation_unit_diagnostics},
    };
    std::ostringstream json_output;
    cxx_dead::write_json_report(json_output, indexed.graph, reachability, report,
                                indexed.diagnostics, indexed_metadata);
    const auto report_json = cxx_dead::json::parse(json_output.str());
    require(report_json.find("findings") != nullptr, "JSON report has no findings field");
    require(report_json.find("schema_version") != nullptr &&
                report_json.find("schema_version")->as_number() == 9.0,
            "JSON report does not use provider schema version 9");
    require(report_json.find("roots") != nullptr && report_json.find("roots")->is_array(),
            "JSON report has no structured roots field");
    require(report_json.find("run") != nullptr &&
                report_json.find("run")->string_or("state") == "complete",
            "JSON report does not identify a complete run");

    std::ostringstream artifact_output;
    cxx_dead::write_graph_artifact(artifact_output, indexed.graph,
                                   {.configuration_id = "default",
                                    .frontend = indexed.frontend,
                                    .translation_units = indexed.translation_units},
                                   indexed.diagnostics);
    const auto artifact_json = cxx_dead::json::parse(artifact_output.str());
    require(artifact_json.find("artifact_schema_version") != nullptr &&
                artifact_json.find("artifact_schema_version")->as_number() == 4.0 &&
                artifact_json.find("identity_schema_version") != nullptr &&
                artifact_json.find("identity_schema_version")->as_number() == 1.0,
            "graph artifact schema versions are missing or coupled to the report schema");
    require(artifact_json.find("symbols") != nullptr && artifact_json.find("symbols")->is_array() &&
                artifact_json.find("edges") != nullptr && artifact_json.find("edges")->is_array(),
            "graph artifact does not contain symbol and edge facts");

    auto reversed_commands = commands;
    std::ranges::reverse(reversed_commands);
    const auto reversed = indexer.index(reversed_commands);
    std::ostringstream reversed_artifact_output;
    cxx_dead::write_graph_artifact(reversed_artifact_output, reversed.graph,
                                   {.configuration_id = "default",
                                    .frontend = reversed.frontend,
                                    .translation_units = reversed.translation_units},
                                   reversed.diagnostics);
    require(artifact_output.str() == reversed_artifact_output.str(),
            "graph artifact depends on translation-unit ordering");
    const auto reversed_reachability = cxx_dead::analyze_reachability(reversed.graph);
    const auto reversed_report = cxx_dead::build_report(reversed.graph, reversed_reachability);
    const cxx_dead::AnalysisMetadata reversed_metadata{
        .run = {.state = cxx_dead::RunState::Complete,
                .frontend = reversed.frontend,
                .partial_graph_discarded = false,
                .translation_units = reversed.translation_unit_diagnostics},
    };
    std::ostringstream reversed_report_output;
    cxx_dead::write_json_report(reversed_report_output, reversed.graph, reversed_reachability,
                                reversed_report, reversed.diagnostics, reversed_metadata);
    require(json_output.str() == reversed_report_output.str(),
            "JSON report depends on translation-unit ordering");

    const auto release_indexed =
        cxx_dead::ClangAstIndexer({.project_root = fixture, .configuration_id = "release"})
            .index(commands);
    const auto default_live = find_symbol(indexed.graph, "live::run");
    const auto release_live = find_symbol(release_indexed.graph, "live::run");
    require(indexed.graph.symbols()[default_live].key !=
                release_indexed.graph.symbols()[release_live].key,
            "different configuration IDs produced the same symbol identity");
    require(indexed.graph.symbols()[default_live].identity.translation_unit.empty(),
            "external-linkage identity was incorrectly scoped to one translation unit");
    const auto internal_dead = find_symbol(indexed.graph, "internal_dead");
    require(!indexed.graph.symbols()[internal_dead].identity.translation_unit.empty(),
            "internal-linkage identity omitted its translation-unit domain");

    std::ostringstream human_output;
    cxx_dead::write_human_report(human_output, indexed.graph, reachability, report,
                                 indexed.diagnostics, indexed_metadata);
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

void test_implicit_construction_integration() {
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
    const auto indexed = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto reachability = cxx_dead::analyze_reachability(indexed.graph);

    require(reachability.reachable[find_symbol(
                indexed.graph, "construction_fixture::DirectProduct::DirectProduct", "void (int)")],
            "direct construction through an alias did not retain the selected constructor");
    require(
        !reachability.reachable[find_symbol(
            indexed.graph, "construction_fixture::DirectProduct::DirectProduct", "void (double)")],
        "direct construction retained an unrelated constructor overload");
    for (const auto signature :
         {std::string_view{"void (int)"}, std::string_view{"void (double)"}}) {
        require(
            reachability.reachable[find_symbol(
                indexed.graph, "construction_fixture::FactoryProduct::FactoryProduct", signature)],
            "smart-pointer factory did not conservatively retain a constructor overload");
    }
    for (const auto name : {
             "construction_fixture::DirectProduct::~DirectProduct",
             "construction_fixture::FactoryProduct::~FactoryProduct",
             "construction_fixture::Base::Base",
             "construction_fixture::Base::~Base",
             "construction_fixture::Member::Member",
             "construction_fixture::Member::~Member",
         }) {
        require(reachability.reachable[find_symbol(indexed.graph, name)],
                "construction or cleanup path was not retained");
    }
    for (const auto name : {
             "construction_fixture::NonFactoryProduct::NonFactoryProduct",
             "construction_fixture::NonFactoryProduct::~NonFactoryProduct",
         }) {
        require(!reachability.reachable[find_symbol(indexed.graph, name)],
                "nested or borrowed owning pointer was treated as a factory result");
    }
    require(
        std::ranges::any_of(
            indexed.diagnostics,
            [](const std::string& diagnostic) {
                return diagnostic.contains("unsupported owning-pointer factory custom_factory") &&
                       diagnostic.contains("conservatively retained construction and destruction");
            }),
        "unsupported owning-pointer factory did not produce a conservative diagnostic");
}

void test_callable_registration_integration() {
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
    const auto indexed = cxx_dead::ClangAstIndexer(options).index(commands);
    const auto reachability = cxx_dead::analyze_reachability(indexed.graph);
    const auto static_function = find_symbol(indexed.graph, "callable_fixture::statically_called");
    const auto escaped_function = find_symbol(indexed.graph, "callable_fixture::escaped_function");
    const auto registered = find_symbol(indexed.graph, "callable_fixture::registered_function");
    const auto member = find_symbol(indexed.graph, "callable_fixture::Handler::member_callback");
    const auto unreachable = find_symbol(indexed.graph, "callable_fixture::unreachable_registered");
    const auto unused = find_symbol(indexed.graph, "callable_fixture::unused_function");
    const auto reassigned_first = find_symbol(indexed.graph, "callable_fixture::reassigned_first");
    const auto reassigned_second =
        find_symbol(indexed.graph, "callable_fixture::reassigned_second");

    require(reachability.structurally_reachable[static_function] &&
                !reachability.provider_reachable[static_function],
            "uniquely bound function pointer call was not structurally reachable");
    require(!reachability.reachable[escaped_function],
            "std::function storage was incorrectly treated as a definite call");
    require(reachability.provider_reachable[registered] && reachability.provider_reachable[member],
            "configured registrar did not retain free and member callbacks as provider reachable");
    require(!reachability.reachable[unreachable],
            "an unreachable registration site retained its callback");
    require(!reachability.reachable[unused], "unused negative control became reachable");
    require(!reachability.reachable[reassigned_first] && !reachability.reachable[reassigned_second],
            "reassigned function pointer was treated as a unique structural call");
    std::optional<cxx_dead::SymbolId> direct_lambda;
    std::optional<cxx_dead::SymbolId> escaped_lambda;
    std::optional<cxx_dead::SymbolId> unused_lambda;
    for (cxx_dead::SymbolId id = 0; id < indexed.graph.symbols().size(); ++id) {
        const auto& symbol = indexed.graph.symbols()[id];
        if (!symbol.defined || symbol.name != "operator()")
            continue;
        switch (cxx_dead::primary_source_extent(symbol).location.line) {
        case 38:
            direct_lambda = id;
            break;
        case 41:
            escaped_lambda = id;
            break;
        case 48:
            unused_lambda = id;
            break;
        default:
            break;
        }
    }
    require(direct_lambda.has_value() && reachability.structurally_reachable[*direct_lambda],
            "direct lambda invocation was not structurally reachable");
    require(escaped_lambda.has_value() && !reachability.reachable[*escaped_lambda],
            "escaped lambda was incorrectly treated as definitely called");
    require(unused_lambda.has_value() && !reachability.reachable[*unused_lambda],
            "unused lambda negative control became reachable");

    const auto report = cxx_dead::build_report(indexed.graph, reachability);
    const auto escaped_finding =
        std::ranges::find_if(report.findings, [&](const cxx_dead::Finding& finding) {
            return finding.symbol == escaped_function;
        });
    const auto unreachable_finding =
        std::ranges::find_if(report.findings, [&](const cxx_dead::Finding& finding) {
            return finding.symbol == unreachable;
        });
    require(escaped_finding != report.findings.end() &&
                escaped_finding->classification == cxx_dead::Classification::DynamicallyReferenced,
            "ambiguous std::function target received a high-confidence classification");
    require(unreachable_finding != report.findings.end() &&
                unreachable_finding->classification ==
                    cxx_dead::Classification::DynamicallyReferenced,
            "callback at an unreachable registrar site lost its escape evidence");
    const auto escaped_lambda_finding =
        std::ranges::find_if(report.findings, [&](const cxx_dead::Finding& finding) {
            return finding.symbol == *escaped_lambda;
        });
    const auto unused_lambda_finding =
        std::ranges::find_if(report.findings, [&](const cxx_dead::Finding& finding) {
            return finding.symbol == *unused_lambda;
        });
    require(escaped_lambda_finding != report.findings.end() &&
                escaped_lambda_finding->classification ==
                    cxx_dead::Classification::DynamicallyReferenced,
            "ambiguous lambda escape received a high-confidence classification");
    require(unused_lambda_finding != report.findings.end() &&
                unused_lambda_finding->classification !=
                    cxx_dead::Classification::DynamicallyReferenced,
            "unused lambda was incorrectly treated as escaped");
    for (const auto reassigned : {reassigned_first, reassigned_second}) {
        const auto finding =
            std::ranges::find_if(report.findings, [&](const cxx_dead::Finding& candidate) {
                return candidate.symbol == reassigned;
            });
        require(finding != report.findings.end() &&
                    finding->classification == cxx_dead::Classification::DynamicallyReferenced,
                "reassigned function-pointer target received a high-confidence classification");
    }
    require(report.provider_reachable.size() == 2U &&
                report.provider_reachable_symbols >= report.provider_reachable.size(),
            "report did not expose provider-reachable callbacks");

    std::ostringstream report_output;
    cxx_dead::write_json_report(report_output, indexed.graph, reachability, report,
                                indexed.diagnostics);
    const auto report_json = cxx_dead::json::parse(report_output.str());
    require(report_json.find("provider_reachable") != nullptr &&
                report_json.find("provider_reachable")->as_array().size() == 2U &&
                report_json.find("summary")->find("provider_reachable_symbols")->as_number() >= 2.0,
            "JSON report omitted provider reachability provenance");

    std::ostringstream artifact_output;
    cxx_dead::write_graph_artifact(artifact_output, indexed.graph,
                                   {.configuration_id = "default",
                                    .frontend = indexed.frontend,
                                    .translation_units = indexed.translation_units},
                                   indexed.diagnostics);
    const auto artifact = cxx_dead::json::parse(artifact_output.str());
    require(artifact.find("artifact_schema_version")->as_number() == 4.0 &&
                std::ranges::any_of(artifact.find("edges")->as_array(),
                                    [](const auto& edge) {
                                        return edge.string_or("kind") == "callback_registration";
                                    }) &&
                std::ranges::any_of(artifact.find("escapes")->as_array(),
                                    [](const auto& escape) {
                                        return escape.string_or("kind") == "callable_object";
                                    }),
            "graph artifact omitted callable provider facts");

    const auto unconfigured = cxx_dead::ClangAstIndexer({.project_root = fixture,
                                                         .ast_filter = "callable_fixture",
                                                         .manual_roots = {"callable_fixture::run"}})
                                  .index(commands);
    const auto unconfigured_reachability = cxx_dead::analyze_reachability(unconfigured.graph);
    require(
        !unconfigured_reachability
             .reachable[find_symbol(unconfigured.graph, "callable_fixture::registered_function")],
        "registration changed reachability without an enabled provider rule");

    try {
        static_cast<void>(
            cxx_dead::ClangAstIndexer(
                {
                    .project_root = fixture,
                    .ast_filter = "callable_fixture",
                    .manual_roots = {"callable_fixture::run"},
                    .callback_registration_rules =
                        {
                            {.callee =
                                 cxx_dead::SymbolSelector{"callable_fixture::missing_registrar"},
                             .argument_index = 0},
                        },
                })
                .index(commands));
        throw std::runtime_error("unmatched callback registration rule unexpectedly succeeded");
    } catch (const cxx_dead::IndexingError& error) {
        require(std::string(error.what()).contains("did not match a registrar call"),
                "unmatched callback registration rule did not fail clearly");
    }
}

void test_yaml_provider_integration() {
    const auto fixture = std::filesystem::path(CXX_DEAD_PROVIDER_FIXTURE_DIR);
    const auto policy = cxx_dead::load_provider_config(fixture / "provider.yaml");
    require(policy.provider == "project_policy" && policy.roots.size() == 2U &&
                policy.edges.size() == 1U && policy.escapes.size() == 1U &&
                policy.suppressions.size() == 1U && policy.callback_registrations.size() == 1U,
            "YAML provider did not load every typed fact");

    for (const auto& [file, expected] : std::vector<std::pair<std::string, std::string>>{
             {"invalid-unknown.yaml", "unknown key"},
             {"invalid-selector.yaml", "exactly one"},
             {"invalid-duplicate.yaml", "duplicate key"},
         }) {
        try {
            static_cast<void>(cxx_dead::load_provider_config(fixture / file));
            throw std::runtime_error("invalid provider configuration unexpectedly loaded: " + file);
        } catch (const std::exception& error) {
            require(std::string(error.what()).contains(expected),
                    "invalid provider configuration did not fail precisely");
        }
    }

    const auto source = fixture / "main.cpp";
    const std::vector<cxx_dead::CompileCommand> commands{{
        .directory = fixture,
        .file = source,
        .arguments = {"clang++", "-std=c++23", "-c", source.string(), "-o", "ignored.o"},
    }};
    auto indexed =
        cxx_dead::ClangAstIndexer({
                                      .project_root = fixture,
                                      .ast_filter = "provider_fixture",
                                      .callback_registration_rules = policy.callback_registrations,
                                      .provider_policies = {policy},
                                  })
            .index(commands);
    const auto reachability = cxx_dead::analyze_reachability(indexed.graph);
    const auto report = cxx_dead::build_report(indexed.graph, reachability);

    const auto plugin_entry = find_symbol(indexed.graph, "provider_fixture::plugin_entry");
    const auto plugin_leaf = find_symbol(indexed.graph, "provider_fixture::plugin_leaf");
    const auto registered = find_symbol(indexed.graph, "provider_fixture::registered_callback");
    const auto escaped = find_symbol(indexed.graph, "provider_fixture::escaped_callback");
    const auto suppressed = find_symbol(indexed.graph, "provider_fixture::suppressed_callback");
    const auto ordinary = find_symbol(indexed.graph, "provider_fixture::ordinary_dead");
    require(reachability.provider_reachable[plugin_entry] &&
                reachability.provider_reachable[plugin_leaf] &&
                reachability.provider_reachable[registered],
            "provider root, edge, or callback registration did not retain its target");
    require(!reachability.reachable[escaped] && !reachability.reachable[suppressed] &&
                !reachability.reachable[ordinary],
            "non-root provider facts unexpectedly changed reachability");
    const auto escaped_finding = std::ranges::find_if(
        report.findings, [&](const auto& finding) { return finding.symbol == escaped; });
    require(escaped_finding != report.findings.end() &&
                escaped_finding->classification == cxx_dead::Classification::DynamicallyReferenced,
            "provider escape did not lower finding confidence");
    require(
        std::ranges::none_of(report.findings,
                             [&](const auto& finding) { return finding.symbol == suppressed; }) &&
            report.suppressed_findings.size() == 1U &&
            report.suppressed_findings.front().finding.symbol == suppressed &&
            report.suppressed_findings.front().suppressions.front().provider == "project_policy" &&
            report.actionable_unreachable_symbols == report.findings.size(),
        "provider suppression was not separated from actionable findings");

    std::ostringstream report_output;
    cxx_dead::write_json_report(report_output, indexed.graph, reachability, report,
                                indexed.diagnostics);
    const auto report_json = cxx_dead::json::parse(report_output.str());
    require(report_json.find("schema_version")->as_number() == 9.0 &&
                report_json.find("suppressed_findings")->as_array().size() == 1U &&
                report_json.find("summary")->find("suppressed_symbols")->as_number() == 1.0,
            "provider report schema omitted auditable suppressions");

    std::ostringstream artifact_output;
    cxx_dead::write_graph_artifact(artifact_output, indexed.graph,
                                   {.configuration_id = "default",
                                    .frontend = indexed.frontend,
                                    .translation_units = indexed.translation_units},
                                   indexed.diagnostics);
    const auto artifact = cxx_dead::json::parse(artifact_output.str());
    require(artifact.find("artifact_schema_version")->as_number() == 4.0 &&
                artifact.find("suppressions")->as_array().size() == 1U,
            "graph artifact omitted provider suppressions");

    for (const auto& [file, expected] : std::vector<std::pair<std::string, std::string>>{
             {"ambiguous.yaml", "matched 2 symbols"},
             {"unmatched.yaml", "matched 0 symbols"},
         }) {
        try {
            auto graph = indexed.graph;
            cxx_dead::apply_provider_policies(graph,
                                              {cxx_dead::load_provider_config(fixture / file)});
            throw std::runtime_error("unresolvable provider selector unexpectedly applied: " +
                                     file);
        } catch (const std::exception& error) {
            require(std::string(error.what()).contains(expected),
                    "unresolvable provider selector did not fail clearly");
        }
    }

    const auto additional = cxx_dead::load_provider_config(fixture / "additional.yaml");
    const auto provider_base =
        cxx_dead::ClangAstIndexer({
                                      .project_root = fixture,
                                      .ast_filter = "provider_fixture",
                                      .manual_roots = {"provider_fixture::run_registration"},
                                      .callback_registration_rules = policy.callback_registrations,
                                  })
            .index(commands)
            .graph;
    auto forward_graph = provider_base;
    auto reverse_graph = provider_base;
    cxx_dead::apply_provider_policies(forward_graph, {policy, additional});
    cxx_dead::apply_provider_policies(reverse_graph, {additional, policy});
    const auto artifact_metadata = cxx_dead::GraphArtifactMetadata{
        .configuration_id = "default",
        .frontend = indexed.frontend,
        .translation_units = indexed.translation_units,
    };
    std::ostringstream forward_output;
    std::ostringstream reverse_output;
    cxx_dead::write_graph_artifact(forward_output, forward_graph, artifact_metadata, {});
    cxx_dead::write_graph_artifact(reverse_output, reverse_graph, artifact_metadata, {});
    require(forward_output.str() == reverse_output.str(),
            "provider file order changed the canonical graph artifact");

    const auto unconfigured =
        cxx_dead::ClangAstIndexer({
                                      .project_root = fixture,
                                      .ast_filter = "provider_fixture",
                                      .manual_roots = {"provider_fixture::run_registration"},
                                  })
            .index(commands);
    const auto unconfigured_reachability = cxx_dead::analyze_reachability(unconfigured.graph);
    require(!unconfigured_reachability
                    .reachable[find_symbol(unconfigured.graph, "provider_fixture::plugin_entry")] &&
                !unconfigured_reachability.reachable[find_symbol(
                    unconfigured.graph, "provider_fixture::registered_callback")],
            "provider fixture changed reachability without configuration");
}

void test_libtooling_availability_contract() {
    if (cxx_dead::libtooling_available())
        return;
    const std::vector<cxx_dead::CompileCommand> commands{{
        .directory = ".",
        .file = "unavailable.cpp",
        .arguments = {"clang++", "-c", "unavailable.cpp"},
    }};
    try {
        static_cast<void>(cxx_dead::LibToolingIndexer({.project_root = "."}).index(commands));
        throw std::runtime_error("unavailable LibTooling frontend unexpectedly ran");
    } catch (const cxx_dead::IndexingError& error) {
        require(std::string(error.what()).contains("CXX_DEAD_ENABLE_LIBTOOLING=ON"),
                "unavailable LibTooling frontend has no actionable diagnostic");
        require(error.diagnostics().state == cxx_dead::RunState::Unsupported &&
                    error.diagnostics().translation_units.size() == 1U &&
                    error.diagnostics().translation_units.front().status ==
                        cxx_dead::TranslationUnitStatus::Unsupported,
                "unavailable frontend did not produce structured unsupported diagnostics");
    }
}

void test_process_limits_and_cancellation() {
    using namespace std::chrono_literals;
    const auto fixture = std::string(CXX_DEAD_PROCESS_FIXTURE);

    const auto timeout_started = std::chrono::steady_clock::now();
    const auto timed_out =
        cxx_dead::run_process({fixture, "spawn"}, std::filesystem::current_path(),
                              {.timeout = 75ms, .termination_grace = 25ms});
    require(timed_out.termination == cxx_dead::ProcessTermination::TimedOut,
            "process timeout did not return a typed termination reason");
    require(std::chrono::steady_clock::now() - timeout_started < 2s,
            "process timeout did not terminate the child process group promptly");

    const auto limited =
        cxx_dead::run_process({fixture, "output"}, std::filesystem::current_path(),
                              {.standard_output_limit = 128U, .termination_grace = 25ms});
    require(limited.termination == cxx_dead::ProcessTermination::OutputLimitExceeded &&
                limited.standard_output.size() == 128U,
            "process output limit was not enforced without retaining excess bytes");

    std::atomic_bool cancelled{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(75ms);
        cancelled.store(true);
    });
    const auto cancelled_result = cxx_dead::run_process(
        {fixture, "spawn"}, std::filesystem::current_path(),
        {.cancellation_requested = [&] { return cancelled.load(); }, .termination_grace = 25ms});
    canceller.join();
    require(cancelled_result.termination == cxx_dead::ProcessTermination::Cancelled,
            "process cancellation did not return a typed termination reason");
}

} // namespace

int main() {
    try {
        test_json();
        test_shell_split();
        test_graph_algorithms();
        test_stable_identity_contract();
        test_clang_integration();
        test_implicit_construction_integration();
        test_callable_registration_integration();
        test_yaml_provider_integration();
        test_process_limits_and_cancellation();
        test_libtooling_availability_contract();
        std::cout << "all cxx-dead tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
