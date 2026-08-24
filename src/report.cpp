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
        if (item.from.has_value())
            output << " from " << graph.symbols()[*item.from].qualified_name;
        output << '\n';
    }
}

std::string location(const Symbol& symbol) {
    if (symbol.file.empty())
        return "<unknown>";
    auto result = symbol.file.string();
    if (symbol.line != 0)
        result += ":" + std::to_string(symbol.line);
    return result;
}

std::map<std::string, std::vector<SymbolId>>
fully_unreachable_types(const Graph& graph, const ReachabilityResult& reachability) {
    std::map<std::string, std::vector<SymbolId>> all_members;
    for (SymbolId id = 0; id < graph.symbols().size(); ++id) {
        const auto& symbol = graph.symbols()[id];
        if (symbol.defined && symbol.project_owned && !symbol.class_name.empty()) {
            all_members[symbol.class_name].push_back(id);
        }
    }
    std::map<std::string, std::vector<SymbolId>> result;
    for (auto& [name, members] : all_members) {
        if (members.size() >= 2U && std::ranges::none_of(members, [&](SymbolId member) {
                return reachability.reachable[member];
            })) {
            std::ranges::sort(members, [&](SymbolId left, SymbolId right) {
                return graph.symbols()[left].line < graph.symbols()[right].line;
            });
            result.emplace(name, std::move(members));
        }
    }
    return result;
}

} // namespace

AnalysisReport build_report(const Graph& graph, const ReachabilityResult& result) {
    AnalysisReport report;
    for (SymbolId id = 0; id < graph.symbols().size(); ++id) {
        const auto& symbol = graph.symbols()[id];
        if (!symbol.defined || !symbol.project_owned || symbol.kind == SymbolKind::Synthetic)
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
        if (lhs.file != rhs.file)
            return lhs.file.string() < rhs.file.string();
        if (lhs.line != rhs.line)
            return lhs.line < rhs.line;
        return lhs.qualified_name < rhs.qualified_name;
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
                        const std::vector<std::string>& diagnostics) {
    output << "cxx-dead application reachability report\n\n"
           << "SUMMARY\n"
           << "  Defined project functions: " << report.defined_symbols << '\n'
           << "  Reachable:                 " << report.reachable_symbols << '\n'
           << "  Unreachable candidates:    " << report.unreachable_symbols << "\n\n";

    output << "ROOTS\n";
    for (const auto& root : graph.roots()) {
        const auto& symbol = graph.symbols()[root.symbol];
        output << "  " << symbol.qualified_name << " [" << to_string(root.kind) << "]\n"
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
                output << "Location: " << location(graph.symbols()[members.front()]) << '\n';
            output << "Members:\n";
            for (const auto member : members) {
                type_members.insert(member);
                const auto& symbol = graph.symbols()[member];
                const auto* finding = find_finding(report, member);
                output << "  - " << symbol.qualified_name << " [" << to_string(symbol.kind) << "]";
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
                output << symbol.qualified_name << '\n'
                       << "  Location:       " << location(symbol) << '\n'
                       << "  Kind:           " << to_string(symbol.kind) << '\n'
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
                       const std::vector<std::string>& diagnostics) {
    std::vector<int> component_by_symbol(graph.symbols().size(), -1);
    for (std::size_t component = 0; component < reachability.unreachable_sccs.size(); ++component) {
        for (const auto symbol : reachability.unreachable_sccs[component]) {
            component_by_symbol[symbol] = static_cast<int>(component);
        }
    }

    output << "{\n"
           << "  \"schema_version\": 2,\n"
           << "  \"mode\": \"application\",\n"
           << "  \"summary\": {\n"
           << "    \"defined_symbols\": " << report.defined_symbols << ",\n"
           << "    \"reachable_symbols\": " << report.reachable_symbols << ",\n"
           << "    \"unreachable_symbols\": " << report.unreachable_symbols << "\n"
           << "  },\n"
           << "  \"roots\": [";
    for (std::size_t index = 0; index < graph.roots().size(); ++index) {
        const auto& root = graph.roots()[index];
        const auto& symbol = graph.symbols()[root.symbol];
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"symbol\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"key\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"kind\": \"" << to_string(root.kind) << "\",\n"
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
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"symbol\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"key\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"kind\": \"" << to_string(symbol.kind) << "\",\n"
               << "      \"file\": \"" << json::escape(symbol.file.string()) << "\",\n"
               << "      \"line\": " << symbol.line << ",\n"
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
                output << ",\n          \"from_symbol\": \""
                       << json::escape(graph.symbols()[*item.from].qualified_name) << "\"";
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

} // namespace cxx_dead
