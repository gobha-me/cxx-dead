#include "cxx_dead/report.h"

#include "cxx_dead/json.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <ostream>
#include <tuple>

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

SourceLineEstimate estimate_lines(const Graph& graph, const std::vector<AggregateMember>& members) {
    std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> ranges_by_file;
    SourceLineEstimate result;
    for (const auto& member : members) {
        const auto& extent = primary_source_extent(graph.symbols()[member.symbol]);
        if (extent.begin.file.empty() || extent.begin.file != extent.end.file ||
            extent.begin.line == 0 || extent.end.line < extent.begin.line) {
            ++result.unmeasured_symbols;
            continue;
        }
        ranges_by_file[extent.begin.file.generic_string()].emplace_back(extent.begin.line,
                                                                        extent.end.line);
    }
    for (auto& [file, ranges] : ranges_by_file) {
        static_cast<void>(file);
        std::ranges::sort(ranges);
        std::size_t begin = ranges.front().first;
        std::size_t end = ranges.front().second;
        for (std::size_t index = 1; index < ranges.size(); ++index) {
            const auto& range = ranges[index];
            if (range.first <= end || range.first - 1U == end) {
                end = std::max(end, range.second);
                continue;
            }
            result.estimated_loc += end - begin + 1U;
            begin = range.first;
            end = range.second;
        }
        result.estimated_loc += end - begin + 1U;
    }
    return result;
}

template <typename Label>
std::vector<OwnershipSummary> summarize_ownership(const Graph& graph,
                                                  const std::vector<AggregateMember>& members,
                                                  Label label_for) {
    std::map<std::string, std::vector<AggregateMember>> grouped;
    for (const auto& member : members) {
        auto label = label_for(graph.symbols()[member.symbol]);
        if (!label.empty())
            grouped[std::move(label)].push_back(member);
    }
    std::vector<OwnershipSummary> summaries;
    summaries.reserve(grouped.size());
    for (auto& [label, grouped_members] : grouped) {
        summaries.push_back({
            .label = std::move(label),
            .members = std::move(grouped_members),
            .lines = {},
        });
        summaries.back().lines = estimate_lines(graph, summaries.back().members);
    }
    return summaries;
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
    std::vector<bool> public_api_root(graph.symbols().size(), false);
    std::vector<bool> suppressed_symbol(graph.symbols().size(), false);
    for (const auto& root : graph.roots()) {
        if (!is_public_api(root.kind))
            continue;
        public_api_root[root.symbol] = true;
        const auto& symbol = graph.symbols()[root.symbol];
        if (is_reportable(symbol.scope)) {
            report.public_api.push_back({
                .symbol = root.symbol,
                .from = std::nullopt,
                .evidence = root.evidence,
            });
        }
    }
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
        if (public_api_root[id])
            ++report.public_api_symbols;
        if (result.reachable[id]) {
            ++report.reachable_symbols;
            if (!public_api_root[id])
                ++report.internal_live_symbols;
            if (result.provider_reachable[id])
                ++report.provider_reachable_symbols;
            else
                ++report.structurally_reachable_symbols;
        } else {
            ++report.unreachable_symbols;
            ++report.internal_unreachable_symbols;
            auto finding = classify(graph, id, symbol);
            std::vector<Evidence> suppressions;
            for (const auto& suppression : graph.suppressions()) {
                if (suppression.symbol == id)
                    suppressions.push_back(suppression.evidence);
            }
            if (suppressions.empty()) {
                ++report.actionable_unreachable_symbols;
                report.findings.push_back(std::move(finding));
            } else {
                ++report.suppressed_symbols;
                suppressed_symbol[id] = true;
                report.suppressed_findings.push_back(
                    {.finding = std::move(finding), .suppressions = std::move(suppressions)});
            }
        }
    }
    for (const auto& edge : graph.edges()) {
        if (!is_provider(edge.kind) || !result.reachable[edge.from] ||
            !result.provider_reachable[edge.to]) {
            continue;
        }
        const auto& symbol = graph.symbols()[edge.to];
        if (symbol.defined && is_reportable(symbol.scope)) {
            report.provider_reachable.push_back({
                .symbol = edge.to,
                .from = edge.from,
                .evidence = edge.evidence,
            });
        }
    }
    for (const auto& root : graph.roots()) {
        if (!is_provider(root.kind) || !result.provider_reachable[root.symbol]) {
            continue;
        }
        const auto& symbol = graph.symbols()[root.symbol];
        if (symbol.defined && is_reportable(symbol.scope)) {
            report.provider_reachable.push_back({
                .symbol = root.symbol,
                .from = std::nullopt,
                .evidence = root.evidence,
            });
        }
    }
    std::ranges::sort(report.provider_reachable, [&](const auto& left, const auto& right) {
        const auto left_from = left.from.has_value() ? graph.symbols()[*left.from].key : "";
        const auto right_from = right.from.has_value() ? graph.symbols()[*right.from].key : "";
        return std::tuple{graph.symbols()[left.symbol].key, left_from, left.evidence.provider,
                          left.evidence.reason} < std::tuple{graph.symbols()[right.symbol].key,
                                                             right_from, right.evidence.provider,
                                                             right.evidence.reason};
    });
    const auto provider_duplicate =
        std::ranges::unique(report.provider_reachable, {}, [](const ProviderReachability& item) {
            return std::tuple{item.symbol, item.from, item.evidence.provider, item.evidence.reason};
        });
    report.provider_reachable.erase(provider_duplicate.begin(), provider_duplicate.end());
    std::ranges::sort(report.public_api, [&](const auto& left, const auto& right) {
        return std::tuple{graph.symbols()[left.symbol].key, left.evidence.provider,
                          left.evidence.reason} < std::tuple{graph.symbols()[right.symbol].key,
                                                             right.evidence.provider,
                                                             right.evidence.reason};
    });
    const auto public_duplicate =
        std::ranges::unique(report.public_api, {}, [](const ProviderReachability& item) {
            return std::tuple{item.symbol, item.evidence.provider, item.evidence.reason};
        });
    report.public_api.erase(public_duplicate.begin(), report.public_api.end());
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
    std::ranges::sort(report.suppressed_findings, [&](const SuppressedFinding& left,
                                                      const SuppressedFinding& right) {
        const auto& lhs = graph.symbols()[left.finding.symbol];
        const auto& rhs = graph.symbols()[right.finding.symbol];
        const auto& lhs_location = primary_source_extent(lhs).location;
        const auto& rhs_location = primary_source_extent(rhs).location;
        return std::tuple{lhs_location.file.string(), lhs_location.line, lhs.qualified_name,
                          lhs.signature} < std::tuple{rhs_location.file.string(), rhs_location.line,
                                                      rhs.qualified_name, rhs.signature};
    });
    std::vector<std::size_t> weak_component_by_scc(result.unreachable_sccs.size());
    for (std::size_t component_id = 0; component_id < result.unreachable_weak_components.size();
         ++component_id) {
        for (const auto scc : result.unreachable_weak_components[component_id])
            weak_component_by_scc[scc] = component_id;
        UnreachableAggregate aggregate{
            .weak_component = component_id,
            .sccs = result.unreachable_weak_components[component_id],
            .edges = {},
            .members = {},
            .lines = {},
            .types = {},
            .files = {},
            .directories = {},
        };
        for (const auto scc : aggregate.sccs) {
            for (const auto symbol : result.unreachable_sccs[scc]) {
                aggregate.members.push_back({
                    .symbol = symbol,
                    .disposition = suppressed_symbol[symbol]
                                       ? AggregateMemberDisposition::Suppressed
                                       : AggregateMemberDisposition::Actionable,
                });
            }
        }
        std::ranges::sort(aggregate.members, [&](const auto& left, const auto& right) {
            return graph.symbols()[left.symbol].key < graph.symbols()[right.symbol].key;
        });
        aggregate.lines = estimate_lines(graph, aggregate.members);
        aggregate.types = summarize_ownership(
            graph, aggregate.members, [](const Symbol& symbol) { return symbol.class_name; });
        aggregate.files = summarize_ownership(graph, aggregate.members, [](const Symbol& symbol) {
            const auto& extent = primary_source_extent(symbol);
            const auto& file =
                extent.location.file.empty() ? extent.begin.file : extent.location.file;
            return file.generic_string();
        });
        aggregate.directories =
            summarize_ownership(graph, aggregate.members, [](const Symbol& symbol) {
                const auto& extent = primary_source_extent(symbol);
                const auto& file =
                    extent.location.file.empty() ? extent.begin.file : extent.location.file;
                return file.empty() ? std::string{} : file.parent_path().generic_string();
            });
        report.unreachable_components.push_back(std::move(aggregate));
    }
    for (const auto& edge : result.unreachable_condensation_edges) {
        const auto component = weak_component_by_scc[edge.from_scc];
        if (component == weak_component_by_scc[edge.to_scc])
            report.unreachable_components[component].edges.push_back(edge);
    }
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
    static_cast<void>(reachability);
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
           << "  Reachable:                 " << report.reachable_symbols << " ("
           << report.structurally_reachable_symbols << " structural, "
           << report.provider_reachable_symbols << " provider)\n"
           << "  Unreachable candidates:    " << report.unreachable_symbols << " ("
           << report.actionable_unreachable_symbols << " actionable, " << report.suppressed_symbols
           << " suppressed)\n"
           << "  Library classes:           " << report.public_api_symbols << " public API, "
           << report.internal_live_symbols << " internal live, "
           << report.internal_unreachable_symbols << " internal unreachable\n\n";

    output << "ROOTS\n";
    for (const auto& root : graph.roots()) {
        const auto& symbol = graph.symbols()[root.symbol];
        output << "  " << display_symbol(symbol) << " [" << to_string(root.kind) << "]\n"
               << "    Scope:    " << to_string(symbol.scope) << '\n'
               << "    Evidence: " << root.evidence.provider << ": " << root.evidence.reason
               << '\n';
    }
    output << '\n';

    if (!report.public_api.empty()) {
        output << "PUBLIC API\n";
        for (const auto& retained : report.public_api) {
            const auto& symbol = graph.symbols()[retained.symbol];
            output << "  " << display_symbol(symbol) << " [externally_reachable]\n"
                   << "    Evidence: " << retained.evidence.provider << ": "
                   << retained.evidence.reason << '\n';
        }
        output << '\n';
    }

    if (!report.provider_reachable.empty()) {
        output << "PROVIDER-REACHABLE SYMBOLS\n";
        for (const auto& retained : report.provider_reachable) {
            const auto& symbol = graph.symbols()[retained.symbol];
            output << "  " << display_symbol(symbol) << "\n";
            if (retained.from.has_value()) {
                output << "    From:     " << display_symbol(graph.symbols()[*retained.from])
                       << "\n";
            }
            output << "    Evidence: " << retained.evidence.provider << ": "
                   << retained.evidence.reason << '\n';
        }
        output << '\n';
    }

    if (report.findings.empty()) {
        output << "No actionable unreachable function definitions found.\n";
    } else {
        for (const auto& component : report.unreachable_components) {
            const auto actionable = static_cast<std::size_t>(
                std::ranges::count_if(component.members, [](const auto& member) {
                    return member.disposition == AggregateMemberDisposition::Actionable;
                }));
            if (actionable == 0)
                continue;
            const auto suppressed = component.members.size() - actionable;
            output << "UNREACHABLE COMPONENT CANDIDATE\n\n"
                   << "  Component:       " << component.weak_component << '\n'
                   << "  SCCs:            " << component.sccs.size() << '\n'
                   << "  Findings:        " << actionable << " actionable, " << suppressed
                   << " suppressed\n"
                   << "  Estimated LOC:   " << component.lines.estimated_loc;
            if (component.lines.unmeasured_symbols != 0)
                output << " (" << component.lines.unmeasured_symbols << " unmeasured symbols)";
            output << "\n  Ownership hints:\n";
            const auto write_ownership = [&](std::string_view kind,
                                             const std::vector<OwnershipSummary>& summaries) {
                for (const auto& summary : summaries) {
                    output << "    - " << kind << ' ' << summary.label << ": "
                           << summary.members.size() << " symbols, estimated "
                           << summary.lines.estimated_loc << " LOC";
                    if (summary.lines.unmeasured_symbols != 0) {
                        output << " (" << summary.lines.unmeasured_symbols << " unmeasured)";
                    }
                    output << '\n';
                }
            };
            write_ownership("type", component.types);
            write_ownership("file", component.files);
            write_ownership("directory", component.directories);
            output << "  Actionable findings:\n";
            for (const auto& member : component.members) {
                if (member.disposition != AggregateMemberDisposition::Actionable)
                    continue;
                const auto& symbol = graph.symbols()[member.symbol];
                const auto* finding = find_finding(report, member.symbol);
                if (finding == nullptr)
                    continue;
                output << "\n    " << display_symbol(symbol) << '\n';
                write_human_source(output, symbol, "      ");
                output << "      Kind:           " << to_string(symbol.kind) << '\n'
                       << "      Classification: " << to_string(finding->classification) << '\n'
                       << "      Confidence:     " << std::fixed << std::setprecision(0)
                       << finding->confidence * 100.0 << "%\n"
                       << "      Evidence:\n";
                write_human_evidence(output, graph, finding->evidence, "        ");
            }
            if (suppressed != 0)
                output << "\n  Suppressed members are audited in SUPPRESSED FINDINGS below.\n";
            output << '\n';
        }
    }

    if (!report.suppressed_findings.empty()) {
        output << "SUPPRESSED FINDINGS\n\n";
        for (const auto& suppressed : report.suppressed_findings) {
            const auto& symbol = graph.symbols()[suppressed.finding.symbol];
            output << display_symbol(symbol) << '\n';
            write_human_source(output, symbol, "  ");
            output << "  Original classification: " << to_string(suppressed.finding.classification)
                   << "\n"
                   << "  Original evidence:\n";
            write_human_evidence(output, graph, suppressed.finding.evidence, "    ");
            output << "  Suppression evidence:\n";
            for (const auto& evidence : suppressed.suppressions)
                output << "    - " << evidence.provider << ": " << evidence.reason << '\n';
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
    std::vector<int> weak_component_by_symbol(graph.symbols().size(), -1);
    for (std::size_t component = 0; component < reachability.unreachable_sccs.size(); ++component) {
        for (const auto symbol : reachability.unreachable_sccs[component]) {
            component_by_symbol[symbol] = static_cast<int>(component);
        }
    }
    for (const auto& component : report.unreachable_components) {
        for (const auto& member : component.members)
            weak_component_by_symbol[member.symbol] = static_cast<int>(component.weak_component);
    }

    const auto write_member_keys = [&](const std::vector<AggregateMember>& members,
                                       AggregateMemberDisposition disposition) {
        output << '[';
        bool first = true;
        for (const auto& member : members) {
            if (member.disposition != disposition)
                continue;
            output << (first ? "" : ", ") << '"' << json::escape(graph.symbols()[member.symbol].key)
                   << '"';
            first = false;
        }
        output << ']';
    };

    const auto write_ownership = [&](const std::vector<OwnershipSummary>& summaries,
                                     std::string_view label_name) {
        output << '[';
        for (std::size_t index = 0; index < summaries.size(); ++index) {
            const auto& summary = summaries[index];
            output << (index == 0 ? "\n" : ",\n") << "        {\n"
                   << "          \"" << label_name << "\": \"" << json::escape(summary.label)
                   << "\",\n"
                   << "          \"estimated_loc\": " << summary.lines.estimated_loc << ",\n"
                   << "          \"unmeasured_symbols\": " << summary.lines.unmeasured_symbols
                   << ",\n"
                   << "          \"finding_keys\": ";
            write_member_keys(summary.members, AggregateMemberDisposition::Actionable);
            output << ",\n          \"suppressed_finding_keys\": ";
            write_member_keys(summary.members, AggregateMemberDisposition::Suppressed);
            output << "\n        }";
        }
        if (!summaries.empty())
            output << '\n' << "      ";
        output << ']';
    };

    const auto write_finding = [&](const Finding& finding,
                                   const std::vector<Evidence>* suppressions) {
        const auto& symbol = graph.symbols()[finding.symbol];
        const auto& source = primary_source_extent(symbol).location;
        output << "    {\n"
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
               << "      \"weak_component\": " << weak_component_by_symbol[finding.symbol] << ",\n"
               << "      \"evidence\": [";
        for (std::size_t index = 0; index < finding.evidence.size(); ++index) {
            const auto& item = finding.evidence[index];
            output << (index == 0 ? "\n" : ",\n") << "        {\n"
                   << "          \"kind\": \"" << to_string(item.kind) << "\",\n"
                   << "          \"provider\": \"" << json::escape(item.evidence.provider)
                   << "\",\n"
                   << "          \"reason\": \"" << json::escape(item.evidence.reason) << "\"";
            if (item.escape_kind.has_value())
                output << ",\n          \"escape_kind\": \"" << to_string(*item.escape_kind)
                       << "\"";
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
        output << "      ]";
        if (suppressions != nullptr) {
            output << ",\n      \"suppression_evidence\": [";
            for (std::size_t index = 0; index < suppressions->size(); ++index) {
                const auto& evidence = (*suppressions)[index];
                output << (index == 0 ? "\n" : ",\n") << "        {\"provider\": \""
                       << json::escape(evidence.provider) << "\", \"reason\": \""
                       << json::escape(evidence.reason) << "\"}";
            }
            if (!suppressions->empty())
                output << '\n';
            output << "      ]";
        }
        output << "\n    }";
    };

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
           << "    \"structurally_reachable_symbols\": " << report.structurally_reachable_symbols
           << ",\n"
           << "    \"provider_reachable_symbols\": " << report.provider_reachable_symbols << ",\n"
           << "    \"public_api_symbols\": " << report.public_api_symbols << ",\n"
           << "    \"internal_live_symbols\": " << report.internal_live_symbols << ",\n"
           << "    \"internal_unreachable_symbols\": " << report.internal_unreachable_symbols
           << ",\n"
           << "    \"unreachable_symbols\": " << report.unreachable_symbols << ",\n"
           << "    \"unreachable_components\": " << report.unreachable_components.size() << ",\n"
           << "    \"actionable_unreachable_symbols\": " << report.actionable_unreachable_symbols
           << ",\n"
           << "    \"suppressed_symbols\": " << report.suppressed_symbols << ",\n"
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
           << "  \"public_api\": [";
    for (std::size_t index = 0; index < report.public_api.size(); ++index) {
        const auto& retained = report.public_api[index];
        const auto& symbol = graph.symbols()[retained.symbol];
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"symbol\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"signature\": \"" << json::escape(symbol.signature) << "\",\n"
               << "      \"key\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"classification\": \"externally_reachable\",\n"
               << "      \"evidence\": {\"provider\": \""
               << json::escape(retained.evidence.provider) << "\", \"reason\": \""
               << json::escape(retained.evidence.reason) << "\"}\n"
               << "    }";
    }
    if (!report.public_api.empty())
        output << '\n';
    output << "  ],\n"
           << "  \"provider_reachable\": [";
    for (std::size_t index = 0; index < report.provider_reachable.size(); ++index) {
        const auto& retained = report.provider_reachable[index];
        const auto& symbol = graph.symbols()[retained.symbol];
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"symbol\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"signature\": \"" << json::escape(symbol.signature) << "\",\n"
               << "      \"key\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"from_symbol\": ";
        if (retained.from.has_value()) {
            const auto& from = graph.symbols()[*retained.from];
            output << '"' << json::escape(from.qualified_name) << "\",\n"
                   << "      \"from_signature\": \"" << json::escape(from.signature) << "\",\n";
        } else {
            output << "null,\n      \"from_signature\": null,\n";
        }
        output << "      \"evidence\": {\"provider\": \""
               << json::escape(retained.evidence.provider) << "\", \"reason\": \""
               << json::escape(retained.evidence.reason) << "\"}\n"
               << "    }";
    }
    if (!report.provider_reachable.empty())
        output << '\n';
    output << "  ],\n"
           << "  \"unreachable_components\": [";
    for (std::size_t index = 0; index < report.unreachable_components.size(); ++index) {
        const auto& component = report.unreachable_components[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"id\": " << component.weak_component << ",\n"
               << "      \"estimated_loc\": " << component.lines.estimated_loc << ",\n"
               << "      \"unmeasured_symbols\": " << component.lines.unmeasured_symbols << ",\n"
               << "      \"sccs\": [";
        for (std::size_t scc_index = 0; scc_index < component.sccs.size(); ++scc_index) {
            const auto scc = component.sccs[scc_index];
            std::vector<AggregateMember> scc_members;
            std::ranges::copy_if(
                component.members, std::back_inserter(scc_members), [&](const auto& member) {
                    return component_by_symbol[member.symbol] == static_cast<int>(scc);
                });
            output << (scc_index == 0 ? "\n" : ",\n") << "        {\n"
                   << "          \"id\": " << scc << ",\n"
                   << "          \"finding_keys\": ";
            write_member_keys(scc_members, AggregateMemberDisposition::Actionable);
            output << ",\n          \"suppressed_finding_keys\": ";
            write_member_keys(scc_members, AggregateMemberDisposition::Suppressed);
            output << "\n        }";
        }
        if (!component.sccs.empty())
            output << '\n';
        output << "      ],\n"
               << "      \"edges\": [";
        for (std::size_t edge_index = 0; edge_index < component.edges.size(); ++edge_index) {
            const auto& edge = component.edges[edge_index];
            output << (edge_index == 0 ? "\n" : ",\n") << "        {\"from_scc\": " << edge.from_scc
                   << ", \"to_scc\": " << edge.to_scc << '}';
        }
        if (!component.edges.empty())
            output << '\n';
        output << "      ],\n"
               << "      \"finding_keys\": ";
        write_member_keys(component.members, AggregateMemberDisposition::Actionable);
        output << ",\n      \"suppressed_finding_keys\": ";
        write_member_keys(component.members, AggregateMemberDisposition::Suppressed);
        output << ",\n      \"ownership\": {\n"
               << "      \"types\": ";
        write_ownership(component.types, "type");
        output << ",\n      \"files\": ";
        write_ownership(component.files, "file");
        output << ",\n      \"directories\": ";
        write_ownership(component.directories, "directory");
        output << "\n      }\n"
               << "    }";
    }
    if (!report.unreachable_components.empty())
        output << '\n';
    output << "  ],\n"
           << "  \"findings\": [";
    for (std::size_t index = 0; index < report.findings.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n");
        write_finding(report.findings[index], nullptr);
    }
    if (!report.findings.empty())
        output << '\n';
    output << "  ],\n  \"suppressed_findings\": [";
    for (std::size_t index = 0; index < report.suppressed_findings.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n");
        const auto& suppressed = report.suppressed_findings[index];
        write_finding(suppressed.finding, &suppressed.suppressions);
    }
    if (!report.suppressed_findings.empty())
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
