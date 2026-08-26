#include "cxx_dead/artifact.h"

#include "cxx_dead/json.h"

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace cxx_dead {

namespace {

void write_source_point(std::ostream& output, const SourcePoint& point) {
    output << "{\"file\": \"" << json::escape(point.file.generic_string())
           << "\", \"line\": " << point.line << ", \"column\": " << point.column
           << ", \"offset\": " << point.offset << ", \"token_length\": " << point.token_length
           << '}';
}

void write_source_extent(std::ostream& output, const SourceExtent& extent) {
    output << "{\"location\": ";
    write_source_point(output, extent.location);
    output << ", \"range\": {\"begin\": ";
    write_source_point(output, extent.begin);
    output << ", \"end\": ";
    write_source_point(output, extent.end);
    output << "}}";
}

void write_source(std::ostream& output, const SymbolSource& source) {
    output << "{\"spelling\": ";
    write_source_extent(output, source.spelling);
    output << ", \"expansion\": ";
    if (source.expansion.has_value())
        write_source_extent(output, *source.expansion);
    else
        output << "null";
    output << '}';
}

void write_evidence(std::ostream& output, const Evidence& evidence) {
    output << "{\"provider\": \"" << json::escape(evidence.provider) << "\", \"reason\": \""
           << json::escape(evidence.reason) << "\"}";
}

std::vector<SymbolId> sorted_symbols(const Graph& graph) {
    std::vector<SymbolId> result(graph.symbols().size());
    for (SymbolId id = 0; id < result.size(); ++id)
        result[id] = id;
    std::ranges::sort(result, [&](SymbolId left, SymbolId right) {
        return graph.symbols()[left].key < graph.symbols()[right].key;
    });
    return result;
}

} // namespace

void write_graph_artifact(std::ostream& output, const Graph& graph,
                          const GraphArtifactMetadata& metadata,
                          const std::vector<std::string>& diagnostics) {
    if (metadata.configuration_id.empty())
        throw std::invalid_argument("graph artifact configuration identity cannot be empty");
    for (const auto& symbol : graph.symbols()) {
        if (symbol.identity.configuration_id != metadata.configuration_id) {
            throw std::invalid_argument(
                "graph artifact configuration does not match symbol identity: " + symbol.key);
        }
    }
    const auto symbol_order = sorted_symbols(graph);
    std::vector<const Edge*> edges;
    edges.reserve(graph.edges().size());
    for (const auto& edge : graph.edges())
        edges.push_back(&edge);
    std::ranges::sort(edges, [&](const Edge* left, const Edge* right) {
        return std::tuple{graph.symbols()[left->from].key, graph.symbols()[left->to].key,
                          left->kind, left->evidence.provider, left->evidence.reason} <
               std::tuple{graph.symbols()[right->from].key, graph.symbols()[right->to].key,
                          right->kind, right->evidence.provider, right->evidence.reason};
    });
    std::vector<const Root*> roots;
    roots.reserve(graph.roots().size());
    for (const auto& root : graph.roots())
        roots.push_back(&root);
    std::ranges::sort(roots, [&](const Root* left, const Root* right) {
        return std::tuple{graph.symbols()[left->symbol].key, left->kind, left->evidence.provider,
                          left->evidence.reason} < std::tuple{graph.symbols()[right->symbol].key,
                                                              right->kind, right->evidence.provider,
                                                              right->evidence.reason};
    });
    std::vector<const Escape*> escapes;
    escapes.reserve(graph.escapes().size());
    for (const auto& escape : graph.escapes())
        escapes.push_back(&escape);
    std::ranges::sort(escapes, [&](const Escape* left, const Escape* right) {
        const auto left_from = left->from.has_value() ? graph.symbols()[*left->from].key : "";
        const auto right_from = right->from.has_value() ? graph.symbols()[*right->from].key : "";
        return std::tuple{graph.symbols()[left->symbol].key, left_from, left->kind,
                          left->evidence.provider, left->evidence.reason} <
               std::tuple{graph.symbols()[right->symbol].key, right_from, right->kind,
                          right->evidence.provider, right->evidence.reason};
    });
    auto sorted_diagnostics = diagnostics;
    std::ranges::sort(sorted_diagnostics);
    const auto duplicate = std::ranges::unique(sorted_diagnostics);
    sorted_diagnostics.erase(duplicate.begin(), duplicate.end());

    output << "{\n"
           << "  \"artifact_schema_version\": " << graph_artifact_schema_version << ",\n"
           << "  \"identity_schema_version\": " << symbol_identity_schema_version << ",\n"
           << "  \"configuration_id\": \"" << json::escape(metadata.configuration_id) << "\",\n"
           << "  \"frontend\": \"" << to_string(metadata.frontend) << "\",\n"
           << "  \"translation_units\": " << metadata.translation_units << ",\n"
           << "  \"symbols\": [";
    for (std::size_t index = 0; index < symbol_order.size(); ++index) {
        const auto& symbol = graph.symbols()[symbol_order[index]];
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"id\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"identity\": {\"quality\": \"" << to_string(symbol.identity.quality)
               << "\", \"configuration_id\": \"" << json::escape(symbol.identity.configuration_id)
               << "\", \"usr\": \"" << json::escape(symbol.identity.usr)
               << "\", \"linkage_name\": \"" << json::escape(symbol.identity.linkage_name)
               << "\", \"translation_unit\": \"" << json::escape(symbol.identity.translation_unit)
               << "\", \"fallback_anchor\": \"" << json::escape(symbol.identity.fallback_anchor)
               << "\"},\n"
               << "      \"name\": \"" << json::escape(symbol.name) << "\",\n"
               << "      \"qualified_name\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"class_name\": \"" << json::escape(symbol.class_name) << "\",\n"
               << "      \"signature\": \"" << json::escape(symbol.signature) << "\",\n"
               << "      \"kind\": \"" << to_string(symbol.kind) << "\",\n"
               << "      \"scope\": \"" << to_string(symbol.scope) << "\",\n"
               << "      \"defined\": " << (symbol.defined ? "true" : "false") << ",\n"
               << "      \"internal_linkage\": " << (symbol.internal_linkage ? "true" : "false")
               << ",\n"
               << "      \"virtual\": " << (symbol.is_virtual ? "true" : "false") << ",\n"
               << "      \"source\": ";
        write_source(output, symbol.source);
        output << "\n    }";
    }
    if (!symbol_order.empty())
        output << '\n';
    output << "  ],\n  \"edges\": [";
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto& edge = *edges[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"from\": \""
               << json::escape(graph.symbols()[edge.from].key) << "\", \"to\": \""
               << json::escape(graph.symbols()[edge.to].key) << "\", \"kind\": \""
               << to_string(edge.kind) << "\", \"evidence\": ";
        write_evidence(output, edge.evidence);
        output << '}';
    }
    if (!edges.empty())
        output << '\n';
    output << "  ],\n  \"roots\": [";
    for (std::size_t index = 0; index < roots.size(); ++index) {
        const auto& root = *roots[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"symbol\": \""
               << json::escape(graph.symbols()[root.symbol].key) << "\", \"kind\": \""
               << to_string(root.kind) << "\", \"evidence\": ";
        write_evidence(output, root.evidence);
        output << '}';
    }
    if (!roots.empty())
        output << '\n';
    output << "  ],\n  \"escapes\": [";
    for (std::size_t index = 0; index < escapes.size(); ++index) {
        const auto& escape = *escapes[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"symbol\": \""
               << json::escape(graph.symbols()[escape.symbol].key) << "\", \"from\": ";
        if (escape.from.has_value())
            output << '"' << json::escape(graph.symbols()[*escape.from].key) << '"';
        else
            output << "null";
        output << ", \"kind\": \"" << to_string(escape.kind) << "\", \"evidence\": ";
        write_evidence(output, escape.evidence);
        output << '}';
    }
    if (!escapes.empty())
        output << '\n';
    output << "  ],\n  \"diagnostics\": [";
    for (std::size_t index = 0; index < sorted_diagnostics.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n") << "    \"" << json::escape(sorted_diagnostics[index])
               << '"';
    }
    if (!sorted_diagnostics.empty())
        output << '\n';
    output << "  ]\n}\n";
}

} // namespace cxx_dead
