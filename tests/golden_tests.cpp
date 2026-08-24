#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"
#include "cxx_dead/indexer.h"
#include "cxx_dead/json.h"
#include "cxx_dead/report.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

struct ExpectedSymbol {
    std::string_view qualified_name;
    std::string_view signature;
    bool reachable;
    std::string_view classification;
    std::string_view secondary_evidence_kind;
    cxx_dead::SymbolKind kind;
    bool internal_linkage{false};
};

std::vector<cxx_dead::SymbolId> matching_symbols(const cxx_dead::Graph& graph,
                                                 std::string_view qualified_name,
                                                 std::string_view signature = {}) {
    std::vector<cxx_dead::SymbolId> matches;
    for (cxx_dead::SymbolId id = 0; id < graph.symbols().size(); ++id) {
        const auto& symbol = graph.symbols()[id];
        if (symbol.defined && symbol.qualified_name == qualified_name &&
            (signature.empty() || symbol.signature == signature)) {
            matches.push_back(id);
        }
    }
    return matches;
}

cxx_dead::SymbolId find_symbol(const cxx_dead::Graph& graph, std::string_view qualified_name,
                               std::string_view signature = {}) {
    const auto matches = matching_symbols(graph, qualified_name, signature);
    if (matches.size() != 1U) {
        std::string message = "expected one definition for " + std::string(qualified_name) +
                              " with signature '" + std::string(signature) + "', found " +
                              std::to_string(matches.size());
        for (const auto& symbol : graph.symbols()) {
            if (symbol.defined && symbol.qualified_name == qualified_name)
                message += "\n  candidate signature: " + symbol.signature;
        }
        throw std::runtime_error(message);
    }
    return matches.front();
}

const cxx_dead::Finding* find_finding(const cxx_dead::AnalysisReport& report,
                                      cxx_dead::SymbolId symbol) {
    const auto found = std::ranges::find_if(report.findings, [=](const cxx_dead::Finding& finding) {
        return finding.symbol == symbol;
    });
    return found == report.findings.end() ? nullptr : &*found;
}

bool has_edge(const cxx_dead::Graph& graph, cxx_dead::SymbolId from, cxx_dead::SymbolId to,
              cxx_dead::EdgeKind kind, std::string_view provider) {
    return std::ranges::any_of(graph.edges(), [=](const cxx_dead::Edge& edge) {
        return edge.from == from && edge.to == to && edge.kind == kind &&
               edge.evidence.provider == provider && !edge.evidence.reason.empty();
    });
}

bool has_root_evidence(const cxx_dead::Graph& graph, cxx_dead::SymbolId symbol,
                       cxx_dead::RootKind kind, std::string_view provider) {
    return std::ranges::any_of(graph.roots(), [&](const cxx_dead::Root& root) {
        return root.symbol == symbol && root.kind == kind && root.evidence.provider == provider &&
               !root.evidence.reason.empty();
    });
}

const cxx_dead::Escape* find_escape(const cxx_dead::Graph& graph, cxx_dead::SymbolId symbol) {
    const auto found = std::ranges::find_if(
        graph.escapes(), [=](const cxx_dead::Escape& escape) { return escape.symbol == symbol; });
    return found == graph.escapes().end() ? nullptr : &*found;
}

void verify_expectation(const cxx_dead::Graph& graph,
                        const cxx_dead::ReachabilityResult& reachability,
                        const cxx_dead::AnalysisReport& report, const ExpectedSymbol& expected) {
    const auto id = find_symbol(graph, expected.qualified_name, expected.signature);
    const auto& symbol = graph.symbols()[id];
    require(reachability.reachable[id] == expected.reachable,
            std::string(expected.qualified_name) + " has the wrong reachability");
    require(symbol.kind == expected.kind,
            std::string(expected.qualified_name) + " has the wrong symbol kind");
    require(symbol.internal_linkage == expected.internal_linkage,
            std::string(expected.qualified_name) + " has the wrong linkage evidence");
    require(symbol.project_owned,
            std::string(expected.qualified_name) + " should be project-owned");
    require(!symbol.file.empty() && symbol.line != 0U,
            std::string(expected.qualified_name) + " should have a source location");

    const auto* finding = find_finding(report, id);
    if (expected.reachable) {
        require(finding == nullptr,
                std::string(expected.qualified_name) + " should not be reported");
    } else {
        require(finding != nullptr,
                std::string(expected.qualified_name) + " should have a finding");
        require(cxx_dead::to_string(finding->classification) == expected.classification,
                std::string(expected.qualified_name) + " has the wrong classification");
        require(!finding->evidence.empty() &&
                    finding->evidence.front().kind ==
                        cxx_dead::FindingEvidenceKind::NoReachablePath &&
                    finding->evidence.front().evidence.provider == "reachability_analysis",
                std::string(expected.qualified_name) + " has no reachability evidence");
        if (expected.secondary_evidence_kind.empty()) {
            require(finding->evidence.size() == 1U,
                    std::string(expected.qualified_name) + " has unexpected extra evidence");
        } else {
            require(finding->evidence.size() >= 2U &&
                        cxx_dead::to_string(finding->evidence[1].kind) ==
                            expected.secondary_evidence_kind,
                    std::string(expected.qualified_name) + " has the wrong secondary evidence");
        }
    }
}

void test_golden_corpus() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const auto started = std::chrono::steady_clock::now();
    const cxx_dead::ClangAstIndexer indexer({.project_root = fixture});
    const auto indexed = indexer.index(commands);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto reachability = cxx_dead::analyze_reachability(indexed.graph);
    const auto report = cxx_dead::build_report(indexed.graph, reachability);

    const std::vector<ExpectedSymbol> expectations{
        {"main", "int ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"overloads::run", "void ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"overloads::select", "void (int)", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"overloads::select",
         "void (double)",
         false,
         "likely_dead",
         {},
         cxx_dead::SymbolKind::Function},
        {"alpha::same_name", "void ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"beta::same_name", "void ()", false, "likely_dead", {}, cxx_dead::SymbolKind::Function},
        {"cross_tu_live", "void ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"cross_tu_dead", "void ()", false, "likely_dead", {}, cxx_dead::SymbolKind::Function},
        {"internal_live", "void ()", true, {}, {}, cxx_dead::SymbolKind::Function, true},
        {"internal_dead", "void ()", false, "dead", "internal_linkage",
         cxx_dead::SymbolKind::Function, true},
        {"(anonymous namespace)::anonymous_live",
         "void ()",
         true,
         {},
         {},
         cxx_dead::SymbolKind::Function,
         true},
        {"(anonymous namespace)::anonymous_dead", "void ()", false, "dead", "internal_linkage",
         cxx_dead::SymbolKind::Function, true},
        {"external_dead", "void ()", false, "likely_dead", {}, cxx_dead::SymbolKind::Function},
        {"LiveBase::LiveBase", "void ()", true, {}, {}, cxx_dead::SymbolKind::Constructor},
        {"LiveBase::~LiveBase", "void () noexcept", true, {}, {}, cxx_dead::SymbolKind::Destructor},
        {"LiveMember::LiveMember", "void ()", true, {}, {}, cxx_dead::SymbolKind::Constructor},
        {"LiveMember::~LiveMember",
         "void () noexcept",
         true,
         {},
         {},
         cxx_dead::SymbolKind::Destructor},
        {"LiveAggregate::LiveAggregate",
         "void ()",
         true,
         {},
         {},
         cxx_dead::SymbolKind::Constructor},
        {"LiveAggregate::~LiveAggregate",
         "void () noexcept",
         true,
         {},
         {},
         cxx_dead::SymbolKind::Destructor},
        {"DeadAggregate::DeadAggregate",
         "void ()",
         false,
         "likely_dead",
         {},
         cxx_dead::SymbolKind::Constructor},
        {"DeadAggregate::~DeadAggregate",
         "void () noexcept",
         false,
         "likely_dead",
         {},
         cxx_dead::SymbolKind::Destructor},
        {"invoke_callback_directly", "void ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"register_callback", "void (Callback)", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"escaped_callback", "void ()", false, "dynamically_referenced", "escape",
         cxx_dead::SymbolKind::Function},
        {"unused_callback", "void ()", false, "likely_dead", {}, cxx_dead::SymbolKind::Function},
        {"templates::transform", "int (int)", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"templates::transform",
         "int (double)",
         false,
         "likely_dead",
         {},
         cxx_dead::SymbolKind::Function},
        {"LiveBase::render", "void ()", true, {}, {}, cxx_dead::SymbolKind::Method},
        {"LiveAggregate::render", "void ()", true, {}, {}, cxx_dead::SymbolKind::Method},
        {"LiveBase::unused_virtual", "void ()", false, "possibly_dead",
         "virtual_dispatch_uncertainty", cxx_dead::SymbolKind::Method},
        {"LiveAggregate::unused_virtual", "void ()", false, "possibly_dead",
         "virtual_dispatch_uncertainty", cxx_dead::SymbolKind::Method},
        {"initialize_global", "int ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"unused_initializer_like",
         "int ()",
         false,
         "likely_dead",
         {},
         cxx_dead::SymbolKind::Function},
        {"macro_live", "void ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"macro_dead", "void ()", false, "likely_dead", {}, cxx_dead::SymbolKind::Function},
        {"generated_live", "void ()", true, {}, {}, cxx_dead::SymbolKind::Function},
        {"generated_dead", "void ()", false, "likely_dead", {}, cxx_dead::SymbolKind::Function},
        {"HeaderApi::unused_static_member",
         "void ()",
         false,
         "likely_dead",
         {},
         cxx_dead::SymbolKind::Method},
    };

    for (const auto& expectation : expectations)
        verify_expectation(indexed.graph, reachability, report, expectation);

    const auto main = find_symbol(indexed.graph, "main", "int ()");
    const auto cross_tu = find_symbol(indexed.graph, "cross_tu_live", "void ()");
    const auto aggregate = find_symbol(indexed.graph, "LiveAggregate::LiveAggregate", "void ()");
    const auto base_render = find_symbol(indexed.graph, "LiveBase::render", "void ()");
    const auto override_render = find_symbol(indexed.graph, "LiveAggregate::render", "void ()");
    const auto escaped = find_symbol(indexed.graph, "escaped_callback", "void ()");
    const auto global = find_symbol(indexed.graph, "initialize_global", "int ()");
    require(has_root_evidence(indexed.graph, main, cxx_dead::RootKind::ApplicationEntryPoint,
                              "application_policy"),
            "main should retain application-root evidence");
    require(has_root_evidence(indexed.graph, global, cxx_dead::RootKind::GlobalInitializer,
                              "clang_ast"),
            "static initialization should retain root evidence");
    require(has_edge(indexed.graph, main, cross_tu, cxx_dead::EdgeKind::DirectCall, "clang_ast"),
            "cross-TU call should be a direct-call edge");
    require(has_edge(indexed.graph, main, aggregate, cxx_dead::EdgeKind::Constructs, "clang_ast"),
            "construction should be a constructs edge");
    require(has_edge(indexed.graph, base_render, override_render,
                     cxx_dead::EdgeKind::VirtualDispatch, "class_hierarchy"),
            "override should be retained by a virtual-dispatch edge");
    const auto* escape = find_escape(indexed.graph, escaped);
    require(escape != nullptr && escape->kind == cxx_dead::EscapeKind::AddressTaken &&
                escape->from == main && escape->evidence.provider == "clang_ast",
            "escaped callback should retain its specific address escape evidence");

    std::ostringstream json_output;
    cxx_dead::write_json_report(json_output, indexed.graph, reachability, report,
                                indexed.diagnostics);
    const auto report_json = cxx_dead::json::parse(json_output.str());
    require(report_json.find("schema_version") != nullptr &&
                report_json.find("schema_version")->as_number() == 2.0,
            "golden JSON report should use schema version 2");
    require(report_json.find("roots") != nullptr &&
                report_json.find("roots")->as_array().size() >= 2U,
            "golden JSON report should expose structured root evidence");
    const auto& findings_json = report_json.find("findings")->as_array();
    const auto escaped_json = std::ranges::find_if(findings_json, [](const auto& finding) {
        return finding.string_or("symbol") == "escaped_callback";
    });
    require(escaped_json != findings_json.end() && escaped_json->find("evidence") != nullptr &&
                escaped_json->find("evidence")->as_array().size() == 2U &&
                escaped_json->find("reason") == nullptr &&
                escaped_json->find("address_taken") == nullptr,
            "escaped callback JSON should expose only the schema-v2 evidence chain");

    const auto macro = indexed.graph.symbols()[find_symbol(indexed.graph, "macro_dead", "void ()")];
    require(macro.file.filename() == "main.cpp", "macro expansion should map to its source file");
    const auto generated =
        indexed.graph.symbols()[find_symbol(indexed.graph, "generated_dead", "void ()")];
    require(generated.file.filename() == "generated.cpp" &&
                generated.file.parent_path().filename() == "generated",
            "generated source should retain its generated path");
    require(matching_symbols(indexed.graph, "HeaderApi::unused_static_member").size() == 1U,
            "external-linkage header definition should merge across translation units");
    require(indexed.translation_units == 3U, "golden corpus should index three translation units");
    require(indexed.ast_bytes > 0U, "golden corpus should record emitted AST bytes");

    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "could not read corpus peak RSS");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cout << "golden-corpus metrics: translation_units=" << indexed.translation_units
              << " ast_bytes=" << indexed.ast_bytes << " symbols=" << indexed.graph.symbols().size()
              << " edges=" << indexed.graph.edges().size() << " findings=" << report.findings.size()
              << " wall_ms=" << elapsed_ms << " peak_rss_kib=" << usage.ru_maxrss << '\n';
}

void test_excluded_generated_path() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    const cxx_dead::ClangAstIndexer indexer({
        .project_root = fixture,
        .excluded_paths = {fixture / "generated"},
    });
    const auto indexed = indexer.index(commands);
    require(matching_symbols(indexed.graph, "generated_live").empty(),
            "excluded generated definitions should not enter the project graph");
    require(matching_symbols(indexed.graph, "generated_dead").empty(),
            "excluded generated findings should not be reportable");
}

void test_incomplete_indexing_fails_closed() {
    const auto fixture = std::filesystem::path(CXX_DEAD_GOLDEN_FIXTURE_DIR);
    const auto source = fixture / "invalid.cpp";
    const std::vector<cxx_dead::CompileCommand> commands{{
        .directory = fixture,
        .file = source,
        .arguments = {"clang++", "-std=c++23", "-c", source.string(), "-o", "invalid.o"},
    }};
    try {
        const cxx_dead::ClangAstIndexer indexer({.project_root = fixture});
        static_cast<void>(indexer.index(commands));
        throw std::runtime_error("invalid translation unit unexpectedly produced an index");
    } catch (const std::exception& error) {
        const std::string message = error.what();
        require(message.contains("Clang AST indexing failed for"),
                "incomplete run should identify indexing failure");
        require(message.contains("invalid.cpp"),
                "incomplete run should identify the failed translation unit");
    }
}

} // namespace

int main() {
    try {
        test_golden_corpus();
        test_excluded_generated_path();
        test_incomplete_indexing_fails_closed();
        std::cout << "all golden corpus tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "golden test failure: " << error.what() << '\n';
        return 1;
    }
}
