#include "cxx_dead/report.h"

#include "cxx_dead/json.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <ostream>
#include <set>

namespace cxx_dead {

namespace {

Finding classify(const Graph& graph, SymbolId id, const Symbol& symbol) {
    Finding finding{
        .symbol = id,
        .classification = Classification::LikelyDead,
        .confidence = 0.95,
        .evidence = {{
            .kind = FindingEvidenceKind::NoReachablePath,
            .evidence = {.provider = "reachability_analysis",
                         .reason = "no path from an application root"},
            .escape_kind = std::nullopt,
            .from = std::nullopt,
        }},
    };

    bool escaped = false;
    for (const auto& escape : graph.escapes()) {
        if (escape.symbol != id)
            continue;
        escaped = true;
        finding.evidence.push_back({
            .kind = FindingEvidenceKind::Escape,
            .evidence = escape.evidence,
            .escape_kind = escape.kind,
            .from = escape.from,
        });
    }

    if (escaped) {
        finding.classification = Classification::DynamicallyReferenced;
        finding.confidence = 0.40;
    } else if (symbol.is_virtual) {
        finding.classification = Classification::PossiblyDead;
        finding.confidence = 0.65;
        finding.evidence.push_back({
            .kind = FindingEvidenceKind::VirtualDispatchUncertainty,
            .evidence = {.provider = "classification_policy",
                         .reason = "virtual dispatch requires conservative review"},
            .escape_kind = std::nullopt,
            .from = std::nullopt,
        });
    } else if (symbol.internal_linkage) {
        finding.classification = Classification::Dead;
        finding.confidence = 0.99;
        finding.evidence.push_back({
            .kind = FindingEvidenceKind::InternalLinkage,
            .evidence = {.provider = "classification_policy",
                         .reason = "symbol has internal linkage"},
            .escape_kind = std::nullopt,
            .from = std::nullopt,
        });
    }
    return finding;
}

const Finding* find_finding(const AnalysisReport& report, SymbolId symbol) {
    const auto found = std::ranges::find_if(
        report.findings, [=](const Finding& finding) { return finding.symbol == symbol; });
    return found == report.findings.end() ? nullptr : &*found;
}

void write_human_evidence(std::ostream& output, const Graph& graph,
                          const std::vector<FindingEvidence>& evidence, std::string_view indent) {
    for (const auto& item : evidence) {
        output << indent << "- [" << to_string(item.kind) << "] " << item.evidence.provider << ": "
               << item.evidence.reason;
        if (item.escape_kind.has_value())
            output << " (" << to_string(*item.escape_kind) << ')';
        if (item.from.has_value()) {
            const auto& source = graph.symbols()[*item.from];
            output << " from " << source.qualified_name << " : " << source.signature;
        }
        output << '\n';
    }
}

std::string location(const SourcePoint& point) {
    if (point.file.empty())
        return "<unknown>";
    auto result = point.file.string();
    if (point.line != 0)
        result += ":" + std::to_string(point.line);
    if (point.column != 0)
        result += ":" + std::to_string(point.column);
    return result;
}

std::string range(const SourceExtent& extent) {
    return location(extent.begin) + " - " + location(extent.end);
}

std::string display_symbol(const Symbol& symbol) {
    return symbol.qualified_name + " : " + symbol.signature;
}

void write_human_source(std::ostream& output, const Symbol& symbol, std::string_view indent) {
    const auto& primary = primary_source_extent(symbol);
    output << indent << "Location:         " << location(primary.location) << '\n'
           << indent << "Definition range: " << range(primary) << '\n';
    if (symbol.source.expansion.has_value()) {
        output << indent << "Spelling:         " << location(symbol.source.spelling.location)
               << '\n'
               << indent << "Expansion:        " << location(symbol.source.expansion->location)
               << '\n';
    }
}

void write_json_source_point(std::ostream& output, const SourcePoint& point) {
    output << "{\"file\": \"" << json::escape(point.file.string()) << "\", \"line\": " << point.line
           << ", \"column\": " << point.column << ", \"offset\": " << point.offset
           << ", \"token_length\": " << point.token_length << '}';
}

void write_json_source_extent(std::ostream& output, const SourceExtent& extent) {
    output << "{\"location\": ";
    write_json_source_point(output, extent.location);
    output << ", \"range\": {\"begin\": ";
    write_json_source_point(output, extent.begin);
    output << ", \"end\": ";
    write_json_source_point(output, extent.end);
    output << "}}";
}

void write_json_source(std::ostream& output, const SymbolSource& source) {
    output << "{\"spelling\": ";
    write_json_source_extent(output, source.spelling);
    output << ", \"expansion\": ";
    if (source.expansion.has_value())
        write_json_source_extent(output, *source.expansion);
    else
        output << "null";
    output << '}';
}

std::map<std::string, std::vector<SymbolId>>
fully_unreachable_types(const Graph& graph, const ReachabilityResult& reachability) {
    std::map<std::string, std::vector<SymbolId>> all_members;
    for (SymbolId id = 0; id < graph.symbols().size(); ++id) {
        const auto& symbol = graph.symbols()[id];
        if (symbol.defined && is_reportable(symbol.scope) && !symbol.class_name.empty()) {
            all_members[symbol.class_name].push_back(id);
        }
    }
    std::map<std::string, std::vector<SymbolId>> result;
    for (auto& [name, members] : all_members) {
        if (members.size() >= 2U && std::ranges::none_of(members, [&](SymbolId member) {
                return reachability.reachable[member];
            })) {
            std::ranges::sort(members, [&](SymbolId left, SymbolId right) {
                return primary_source_extent(graph.symbols()[left]).location.line <
                       primary_source_extent(graph.symbols()[right]).location.line;
            });
            result.emplace(name, std::move(members));
        }
    }
    return result;
}

void write_json_run(std::ostream& output, const RunDiagnostics& run) {
    output << "  \"run\": {\n"
           << "    \"state\": \"" << to_string(run.state) << "\",\n"
           << "    \"frontend\": \"" << to_string(run.frontend) << "\",\n"
           << "    \"partial_graph_discarded\": "
           << (run.partial_graph_discarded ? "true" : "false") << ",\n"
           << "    \"translation_units\": [";
    for (std::size_t index = 0; index < run.translation_units.size(); ++index) {
        const auto& unit = run.translation_units[index];
        output << (index == 0 ? "\n" : ",\n") << "      {\n"
               << "        \"file\": \"" << json::escape(unit.file.string()) << "\",\n"
               << "        \"status\": \"" << to_string(unit.status) << "\",\n"
               << "        \"stage\": \"" << json::escape(unit.stage) << "\",\n"
               << "        \"message\": \"" << json::escape(unit.message) << "\"";
        if (unit.exit_code.has_value())
            output << ",\n        \"exit_code\": " << *unit.exit_code;
        if (unit.signal.has_value() && *unit.signal != 0)
            output << ",\n        \"signal\": " << *unit.signal;
        output << "\n      }";
    }
    if (!run.translation_units.empty())
        output << '\n';
    output << "    ]\n  }";
}

} // namespace

AnalysisReport build_report(const Graph& graph, const ReachabilityResult& result) {
    AnalysisReport report;
    for (SymbolId id = 0; id < graph.symbols().size(); ++id) {
        const auto& symbol = graph.symbols()[id];
        switch (symbol.scope) {
        case SymbolScope::Reportable:
            ++report.reportable_symbols;
            break;
        case SymbolScope::Indexed:
            ++report.indexed_symbols;
            break;
        case SymbolScope::ExternalOpaque:
            ++report.external_opaque_symbols;
            break;
        case SymbolScope::Excluded:
            break;
        }
        if (!symbol.defined || !is_reportable(symbol.scope) || symbol.kind == SymbolKind::Synthetic)
            continue;
        ++report.defined_symbols;
        if (result.reachable[id]) {
            ++report.reachable_symbols;
        } else {
            ++report.unreachable_symbols;
            report.findings.push_back(classify(graph, id, symbol));
        }
    }
    std::ranges::sort(report.findings, [&](const Finding& left, const Finding& right) {
        const auto& lhs = graph.symbols()[left.symbol];
        const auto& rhs = graph.symbols()[right.symbol];
        const auto& lhs_location = primary_source_extent(lhs).location;
        const auto& rhs_location = primary_source_extent(rhs).location;
        if (lhs_location.file != rhs_location.file)
            return lhs_location.file.string() < rhs_location.file.string();
        if (lhs_location.line != rhs_location.line)
            return lhs_location.line < rhs_location.line;
        if (lhs.qualified_name != rhs.qualified_name)
            return lhs.qualified_name < rhs.qualified_name;
        return lhs.signature < rhs.signature;
    });
    return report;
}

std::string_view to_string(Classification classification) {
    switch (classification) {
    case Classification::Dead:
        return "dead";
    case Classification::LikelyDead:
        return "likely_dead";
    case Classification::PossiblyDead:
        return "possibly_dead";
    case Classification::DynamicallyReferenced:
        return "dynamically_referenced";
    }
    return "unknown";
}

std::string_view to_string(FindingEvidenceKind kind) {
    switch (kind) {
    case FindingEvidenceKind::NoReachablePath:
        return "no_reachable_path";
    case FindingEvidenceKind::InternalLinkage:
        return "internal_linkage";
    case FindingEvidenceKind::VirtualDispatchUncertainty:
        return "virtual_dispatch_uncertainty";
    case FindingEvidenceKind::Escape:
        return "escape";
    }
    return "unknown";
}

void write_human_report(std::ostream& output, const Graph& graph,
                        const ReachabilityResult& reachability, const AnalysisReport& report,
                        const std::vector<std::string>& diagnostics,
                        const AnalysisMetadata& metadata) {
    output << "Run state: " << to_string(metadata.run.state) << " ("
           << to_string(metadata.run.frontend) << ", " << metadata.run.translation_units.size()
           << " translation units)\n\n";
    if (metadata.mode == "target") {
        output << "Analysis target: " << metadata.target_name << " (" << metadata.target_kind
               << ")\n"
               << "Configuration: " << metadata.configuration << " [" << metadata.configuration_id
               << "]\n"
               << "Target closure: ";
        for (std::size_t index = 0; index < metadata.closure_targets.size(); ++index) {
            if (index != 0)
                output << ", ";
            output << metadata.closure_targets[index];
        }
        output << "\n\n";
    }
    output << "cxx-dead application reachability report\n\n"
           << "SUMMARY\n"
           << "  Graph symbols by scope:    " << report.reportable_symbols << " reportable, "
           << report.indexed_symbols << " indexed, " << report.external_opaque_symbols
           << " external opaque\n"
           << "  Defined project functions: " << report.defined_symbols << '\n'
           << "  Reachable:                 " << report.reachable_symbols << '\n'
           << "  Unreachable candidates:    " << report.unreachable_symbols << "\n\n";

    output << "ROOTS\n";
    for (const auto& root : graph.roots()) {
        const auto& symbol = graph.symbols()[root.symbol];
        output << "  " << display_symbol(symbol) << " [" << to_string(root.kind) << "]\n"
               << "    Scope:    " << to_string(symbol.scope) << '\n'
               << "    Evidence: " << root.evidence.provider << ": " << root.evidence.reason
               << '\n';
    }
    output << '\n';

    if (report.findings.empty()) {
        output << "No unreachable function definitions found.\n";
    } else {
        const auto dead_types = fully_unreachable_types(graph, reachability);
        std::set<SymbolId> type_members;
        for (const auto& [type, members] : dead_types) {
            output << "UNREACHABLE TYPE\n\n" << type << '\n';
            if (!members.empty())
                output << "Location: "
                       << location(primary_source_extent(graph.symbols()[members.front()]).location)
                       << '\n';
            output << "Members:\n";
            for (const auto member : members) {
                type_members.insert(member);
                const auto& symbol = graph.symbols()[member];
                const auto* finding = find_finding(report, member);
                output << "  - " << display_symbol(symbol) << " [" << to_string(symbol.kind) << "]";
                if (finding != nullptr)
                    output << " " << to_string(finding->classification);
                output << '\n';
                if (finding != nullptr)
                    write_human_evidence(output, graph, finding->evidence, "      ");
            }
            output << '\n';
        }

        for (const auto& component : reachability.unreachable_sccs) {
            std::vector<SymbolId> visible;
            std::ranges::copy_if(component, std::back_inserter(visible),
                                 [&](SymbolId id) { return !type_members.contains(id); });
            if (visible.empty())
                continue;
            output << (visible.size() > 1U ? "UNREACHABLE COMPONENT\n\n"
                                           : "UNREACHABLE SYMBOL\n\n");
            for (const auto id : visible) {
                const auto& symbol = graph.symbols()[id];
                const auto* finding = find_finding(report, id);
                if (finding == nullptr)
                    continue;
                output << display_symbol(symbol) << '\n';
                write_human_source(output, symbol, "  ");
                output << "  Kind:           " << to_string(symbol.kind) << '\n'
                       << "  Classification: " << to_string(finding->classification) << '\n'
                       << "  Confidence:     " << std::fixed << std::setprecision(0)
                       << finding->confidence * 100.0 << "%\n"
                       << "  Evidence:\n";
                write_human_evidence(output, graph, finding->evidence, "    ");
            }
            if (visible.size() > 1U) {
                output << "  SCC size:       " << visible.size() << " mutually reachable symbols\n";
            }
            output << '\n';
        }
    }

    if (!diagnostics.empty()) {
        output << "DIAGNOSTICS\n";
        for (const auto& diagnostic : diagnostics)
            output << "  - " << diagnostic << '\n';
    }
}

void write_json_report(std::ostream& output, const Graph& graph,
                       const ReachabilityResult& reachability, const AnalysisReport& report,
                       const std::vector<std::string>& diagnostics,
                       const AnalysisMetadata& metadata) {
    std::vector<int> component_by_symbol(graph.symbols().size(), -1);
    for (std::size_t component = 0; component < reachability.unreachable_sccs.size(); ++component) {
        for (const auto symbol : reachability.unreachable_sccs[component]) {
            component_by_symbol[symbol] = static_cast<int>(component);
        }
    }

    output << "{\n"
           << "  \"schema_version\": " << report_schema_version << ",\n"
           << "  \"mode\": \"" << json::escape(metadata.mode) << "\",\n";
    write_json_run(output, metadata.run);
    output << ",\n  \"analysis_context\": {\n"
           << "    \"configuration_id\": \"" << json::escape(metadata.configuration_id) << "\",\n"
           << "    \"configuration\": \"" << json::escape(metadata.configuration) << "\",\n"
           << "    \"target_id\": ";
    if (metadata.target_id.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_id) << '"';
    output << ",\n    \"target_name\": ";
    if (metadata.target_name.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_name) << '"';
    output << ",\n    \"target_kind\": ";
    if (metadata.target_kind.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_kind) << '"';
    output << ",\n    \"closure_targets\": [";
    for (std::size_t index = 0; index < metadata.closure_targets.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << '"' << json::escape(metadata.closure_targets[index]) << '"';
    }
    output << "]\n  },\n"
           << "  \"summary\": {\n"
           << "    \"defined_symbols\": " << report.defined_symbols << ",\n"
           << "    \"reachable_symbols\": " << report.reachable_symbols << ",\n"
           << "    \"unreachable_symbols\": " << report.unreachable_symbols << ",\n"
           << "    \"scope_counts\": {\n"
           << "      \"reportable\": " << report.reportable_symbols << ",\n"
           << "      \"indexed\": " << report.indexed_symbols << ",\n"
           << "      \"external_opaque\": " << report.external_opaque_symbols << "\n"
           << "    }\n"
           << "  },\n"
           << "  \"roots\": [";
    for (std::size_t index = 0; index < graph.roots().size(); ++index) {
        const auto& root = graph.roots()[index];
        const auto& symbol = graph.symbols()[root.symbol];
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"symbol\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"signature\": \"" << json::escape(symbol.signature) << "\",\n"
               << "      \"key\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"kind\": \"" << to_string(root.kind) << "\",\n"
               << "      \"scope\": \"" << to_string(symbol.scope) << "\",\n"
               << "      \"source\": ";
        write_json_source(output, symbol.source);
        output << ",\n"
               << "      \"evidence\": {\n"
               << "        \"provider\": \"" << json::escape(root.evidence.provider) << "\",\n"
               << "        \"reason\": \"" << json::escape(root.evidence.reason) << "\"\n"
               << "      }\n"
               << "    }";
    }
    if (!graph.roots().empty())
        output << '\n';
    output << "  ],\n"
           << "  \"findings\": [";
    for (std::size_t index = 0; index < report.findings.size(); ++index) {
        const auto& finding = report.findings[index];
        const auto& symbol = graph.symbols()[finding.symbol];
        const auto& source = primary_source_extent(symbol).location;
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"symbol\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"signature\": \"" << json::escape(symbol.signature) << "\",\n"
               << "      \"key\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"kind\": \"" << to_string(symbol.kind) << "\",\n"
               << "      \"scope\": \"" << to_string(symbol.scope) << "\",\n"
               << "      \"file\": \"" << json::escape(source.file.string()) << "\",\n"
               << "      \"line\": " << source.line << ",\n"
               << "      \"source\": ";
        write_json_source(output, symbol.source);
        output << ",\n"
               << "      \"classification\": \"" << to_string(finding.classification) << "\",\n"
               << "      \"confidence\": " << std::fixed << std::setprecision(2)
               << finding.confidence << ",\n"
               << "      \"component\": " << component_by_symbol[finding.symbol] << ",\n"
               << "      \"evidence\": [";
        for (std::size_t evidence_index = 0; evidence_index < finding.evidence.size();
             ++evidence_index) {
            const auto& item = finding.evidence[evidence_index];
            output << (evidence_index == 0 ? "\n" : ",\n") << "        {\n"
                   << "          \"kind\": \"" << to_string(item.kind) << "\",\n"
                   << "          \"provider\": \"" << json::escape(item.evidence.provider)
                   << "\",\n"
                   << "          \"reason\": \"" << json::escape(item.evidence.reason) << "\"";
            if (item.escape_kind.has_value()) {
                output << ",\n          \"escape_kind\": \"" << to_string(*item.escape_kind)
                       << "\"";
            }
            if (item.from.has_value()) {
                const auto& from = graph.symbols()[*item.from];
                output << ",\n          \"from_symbol\": \"" << json::escape(from.qualified_name)
                       << "\",\n"
                       << "          \"from_signature\": \"" << json::escape(from.signature)
                       << "\"";
            }
            output << "\n        }";
        }
        if (!finding.evidence.empty())
            output << '\n';
        output << "      ]\n"
               << "    }";
    }
    if (!report.findings.empty())
        output << '\n';
    output << "  ],\n  \"diagnostics\": [";
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n") << "    \"" << json::escape(diagnostics[index])
               << "\"";
    }
    if (!diagnostics.empty())
        output << '\n';
    output << "  ]\n}\n";
}

void write_human_run_diagnostic(std::ostream& output, const IndexingError& error,
                                const AnalysisMetadata&) {
    const auto& run = error.diagnostics();
    output << "cxx-dead indexing run\n\n"
           << "RUN STATE\n"
           << "  State:                   " << to_string(run.state) << '\n'
           << "  Frontend:                " << to_string(run.frontend) << '\n'
           << "  Partial graph discarded: " << (run.partial_graph_discarded ? "yes" : "no")
           << "\n\n"
           << "TRANSLATION UNITS\n";
    for (const auto& unit : run.translation_units) {
        output << "  - " << unit.file.string() << " [" << to_string(unit.status) << "]\n"
               << "    Stage: " << unit.stage << '\n'
               << "    Cause: " << unit.message << '\n';
        if (unit.exit_code.has_value())
            output << "    Exit:  " << *unit.exit_code << '\n';
        if (unit.signal.has_value() && *unit.signal != 0)
            output << "    Signal: " << *unit.signal << '\n';
    }
    output << "\nDIAGNOSTIC\n  " << error.what() << '\n';
}

void write_json_run_diagnostic(std::ostream& output, const IndexingError& error,
                               const AnalysisMetadata& metadata) {
    output << "{\n"
           << "  \"schema_version\": " << report_schema_version << ",\n"
           << "  \"mode\": \"" << json::escape(metadata.mode) << "\",\n";
    write_json_run(output, error.diagnostics());
    output << ",\n"
           << "  \"analysis_context\": {\n"
           << "    \"configuration_id\": \"" << json::escape(metadata.configuration_id) << "\",\n"
           << "    \"configuration\": \"" << json::escape(metadata.configuration) << "\",\n"
           << "    \"target_id\": ";
    if (metadata.target_id.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_id) << '"';
    output << ",\n    \"target_name\": ";
    if (metadata.target_name.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_name) << '"';
    output << ",\n    \"target_kind\": ";
    if (metadata.target_kind.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_kind) << '"';
    output << ",\n    \"closure_targets\": [";
    for (std::size_t index = 0; index < metadata.closure_targets.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << '"' << json::escape(metadata.closure_targets[index]) << '"';
    }
    output << "]\n  },\n"
           << "  \"diagnostics\": [\"" << json::escape(error.what()) << "\"]\n"
           << "}\n";
}

} // namespace cxx_dead
