#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"
#include "cxx_dead/indexer.h"
#include "cxx_dead/json.h"
#include "cxx_dead/report.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

cxx_dead::SymbolId find_symbol(const cxx_dead::Graph& graph, std::string_view qualified_name) {
    const auto found = std::ranges::find_if(graph.symbols(), [&](const cxx_dead::Symbol& symbol) {
        return symbol.qualified_name == qualified_name;
    });
    if (found == graph.symbols().end())
        throw std::runtime_error("missing symbol: " + std::string(qualified_name));
    return static_cast<cxx_dead::SymbolId>(std::distance(graph.symbols().begin(), found));
}

const cxx_dead::Finding* find_finding(const cxx_dead::AnalysisReport& report,
                                      cxx_dead::SymbolId symbol) {
    const auto found = std::ranges::find_if(report.findings, [=](const cxx_dead::Finding& finding) {
        return finding.symbol == symbol;
    });
    return found == report.findings.end() ? nullptr : &*found;
}

void test_scope_separation() {
    const auto fixture = std::filesystem::path(CXX_DEAD_SCOPE_FIXTURE_DIR);
    const auto commands = cxx_dead::load_compilation_database(fixture / "compile_commands.json");
    bool outside_report_path_rejected = false;
    try {
        static_cast<void>(cxx_dead::ClangAstIndexer({
            .project_root = fixture,
            .report_paths = {fixture.parent_path()},
        }));
    } catch (const std::invalid_argument&) {
        outside_report_path_rejected = true;
    }
    require(outside_report_path_rejected, "report path outside the project root was accepted");

    const auto started = std::chrono::steady_clock::now();
    const cxx_dead::ClangAstIndexer indexer({
        .project_root = fixture,
        .report_paths = {fixture / "app"},
    });
    const auto indexed = indexer.index(commands);
    const auto reachability = cxx_dead::analyze_reachability(indexed.graph);
    const auto report = cxx_dead::build_report(indexed.graph, reachability);

    const auto framework_run = find_symbol(indexed.graph, "framework::Application::run");
    const auto unused_framework = find_symbol(indexed.graph, "framework::unused_framework");
    const auto callback = find_symbol(indexed.graph, "NullVectorApp::on_start");
    const auto live_helper = find_symbol(indexed.graph, "live_helper");
    const auto dead_helper = find_symbol(indexed.graph, "dead_helper");
    const auto external = find_symbol(indexed.graph, "external_api");

    require(indexed.graph.symbols()[framework_run].scope == cxx_dead::SymbolScope::Indexed &&
                reachability.reachable[framework_run] &&
                find_finding(report, framework_run) == nullptr,
            "framework implementation should propagate reachability without being reportable");
    require(indexed.graph.symbols()[unused_framework].scope == cxx_dead::SymbolScope::Indexed &&
                !reachability.reachable[unused_framework] &&
                find_finding(report, unused_framework) == nullptr,
            "unreachable indexed framework definition should not be reported");
    require(indexed.graph.symbols()[callback].scope == cxx_dead::SymbolScope::Reportable &&
                reachability.reachable[callback] && reachability.reachable[live_helper],
            "framework virtual dispatch did not reach the application callback cascade");
    require(!reachability.reachable[dead_helper] && find_finding(report, dead_helper) != nullptr,
            "unreachable reportable helper should remain a finding");
    require(indexed.graph.symbols()[external].scope == cxx_dead::SymbolScope::ExternalOpaque &&
                reachability.reachable[external] && !indexed.graph.symbols()[external].defined &&
                find_finding(report, external) == nullptr,
            "external reference should be a reachable opaque terminal");
    require(report.findings.size() == 1U && report.indexed_symbols >= 2U &&
                report.external_opaque_symbols == 1U,
            "scope-separated report has unexpected candidates or scope counts");

    std::ostringstream json_output;
    cxx_dead::write_json_report(json_output, indexed.graph, reachability, report,
                                indexed.diagnostics);
    const auto json_report = cxx_dead::json::parse(json_output.str());
    const auto* summary = json_report.find("summary");
    const auto* scope_counts = summary == nullptr ? nullptr : summary->find("scope_counts");
    require(json_report.find("schema_version")->as_number() == 11.0 && scope_counts != nullptr &&
                scope_counts->find("indexed")->as_number() >= 2.0,
            "JSON report does not expose stable-identity scope counts");
    const auto& findings = json_report.find("findings")->as_array();
    require(findings.size() == 1U && findings.front().string_or("scope") == "reportable",
            "JSON finding does not identify report ownership");
    const auto& roots = json_report.find("roots")->as_array();
    require(!roots.empty() && roots.front().string_or("scope") == "reportable",
            "JSON root does not identify symbol scope");

    std::ostringstream human_output;
    cxx_dead::write_human_report(human_output, indexed.graph, reachability, report,
                                 indexed.diagnostics);
    require(human_output.str().contains("Graph symbols by scope") &&
                human_output.str().contains("Scope:    reportable"),
            "human report does not expose scope information");

    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "could not read scope-fixture peak RSS");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    std::cout << "scope-fixture metrics: translation_units=" << indexed.translation_units
              << " ast_bytes=" << indexed.ast_bytes << " symbols=" << indexed.graph.symbols().size()
              << " fact_bytes=" << indexed.fact_bytes << " edges=" << indexed.graph.edges().size()
              << " findings=" << report.findings.size() << " wall_ms=" << elapsed_ms
              << " peak_rss_kib=" << usage.ru_maxrss << '\n';
}

} // namespace

int main() {
    try {
        test_scope_separation();
        std::cout << "all symbol scope tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scope test failure: " << error.what() << '\n';
        return 1;
    }
}
