#include "cxx_dead/graph.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace cxx_dead {

namespace {

int scope_rank(SymbolScope scope) {
    switch (scope) {
    case SymbolScope::Reportable:
        return 3;
    case SymbolScope::Indexed:
        return 2;
    case SymbolScope::ExternalOpaque:
        return 1;
    case SymbolScope::Excluded:
        return 0;
    }
    return 0;
}

void merge_symbol(Symbol& target, Symbol source) {
    const bool target_was_defined = target.defined;
    const auto require_compatible = [&](bool compatible, std::string_view field) {
        if (!compatible) {
            throw std::runtime_error("incompatible " + std::string(field) +
                                     " for stable symbol identity " + target.key);
        }
    };
    require_compatible(target.kind == source.kind, "kind");
    require_compatible(target.qualified_name.empty() || source.qualified_name.empty() ||
                           target.qualified_name == source.qualified_name,
                       "qualified name");
    require_compatible(target.name.empty() || source.name.empty() || target.name == source.name,
                       "name");
    require_compatible(target.class_name.empty() || source.class_name.empty() ||
                           target.class_name == source.class_name,
                       "owning class");
    require_compatible(target.internal_linkage == source.internal_linkage, "linkage domain");
    require_compatible(target.identity.configuration_id == source.identity.configuration_id,
                       "configuration identity");
    require_compatible(target.identity.linkage_name == source.identity.linkage_name,
                       "linkage identity");
    require_compatible(target.identity.translation_unit == source.identity.translation_unit,
                       "translation-unit identity");
    require_compatible(target.identity.usr.empty() || source.identity.usr.empty() ||
                           target.identity.usr == source.identity.usr,
                       "Clang USR");
    if (target.identity.usr.empty())
        target.identity.usr = source.identity.usr;
    if (target.identity.fallback_anchor.empty() ||
        (!source.identity.fallback_anchor.empty() &&
         source.identity.fallback_anchor < target.identity.fallback_anchor)) {
        target.identity.fallback_anchor = source.identity.fallback_anchor;
    }
    if (source.identity.quality == IdentityQuality::Stable)
        target.identity.quality = IdentityQuality::Stable;

    const auto target_scope = target.scope;
    target.defined = target.defined || source.defined;
    if (scope_rank(source.scope) > scope_rank(target.scope))
        target.scope = source.scope;
    target.internal_linkage = target.internal_linkage || source.internal_linkage;
    target.is_virtual = target.is_virtual || source.is_virtual;
    const bool source_has_preferred_definition = !target_was_defined && source.defined;
    const bool source_has_preferred_scope =
        target_was_defined == source.defined && scope_rank(source.scope) > scope_rank(target_scope);
    const auto source_order = [](const SymbolSource& value) {
        const auto& extent = value.expansion.has_value() ? *value.expansion : value.spelling;
        return std::tuple{extent.location.file.generic_string(), extent.location.line,
                          extent.location.column, extent.location.offset};
    };
    const bool source_is_canonical = source_order(source.source) < source_order(target.source);
    const bool source_is_canonical_peer =
        target_was_defined == source.defined && target_scope == source.scope && source_is_canonical;
    if ((primary_source_extent(target).location.file.empty() || source_has_preferred_definition ||
         source_has_preferred_scope || source_is_canonical_peer) &&
        !primary_source_extent(source).location.file.empty()) {
        target.source = std::move(source.source);
    }
    if (target.qualified_name.empty())
        target.qualified_name = std::move(source.qualified_name);
    if (target.class_name.empty())
        target.class_name = std::move(source.class_name);
    if (target.signature.empty() || source_has_preferred_definition || source_has_preferred_scope ||
        source_is_canonical_peer) {
        target.signature = std::move(source.signature);
    }
}

} // namespace

SymbolId Graph::add_or_merge_symbol(Symbol symbol) {
    if (symbol.key.empty()) {
        throw std::invalid_argument("symbol key cannot be empty");
    }
    if (symbol.scope == SymbolScope::Excluded)
        throw std::invalid_argument("excluded symbols cannot be added to the graph");
    if (const auto existing = key_to_id_.find(symbol.key); existing != key_to_id_.end()) {
        merge_symbol(symbols_[existing->second], std::move(symbol));
        return existing->second;
    }
    const auto id = symbols_.size();
    key_to_id_.emplace(symbol.key, id);
    symbols_.push_back(std::move(symbol));
    return id;
}

void Graph::add_edge(SymbolId from, SymbolId to, EdgeKind kind, Evidence evidence) {
    if (from >= symbols_.size() || to >= symbols_.size()) {
        throw std::out_of_range("graph edge references an invalid symbol");
    }
    const auto duplicate = std::ranges::any_of(edges_, [&](const Edge& edge) {
        return edge.from == from && edge.to == to && edge.kind == kind && edge.evidence == evidence;
    });
    if (!duplicate)
        edges_.push_back({from, to, kind, std::move(evidence)});
}

void Graph::add_root(SymbolId id, RootKind kind, Evidence evidence) {
    if (id >= symbols_.size())
        throw std::out_of_range("invalid graph root");
    const auto duplicate = std::ranges::any_of(roots_, [&](const Root& root) {
        return root.symbol == id && root.kind == kind && root.evidence == evidence;
    });
    if (!duplicate)
        roots_.push_back({id, kind, std::move(evidence)});
}

void Graph::add_escape(SymbolId id, EscapeKind kind, Evidence evidence,
                       std::optional<SymbolId> from) {
    if (id >= symbols_.size() || (from.has_value() && *from >= symbols_.size()))
        throw std::out_of_range("graph escape references an invalid symbol");
    const auto duplicate = std::ranges::any_of(escapes_, [&](const Escape& escape) {
        return escape.symbol == id && escape.from == from && escape.kind == kind &&
               escape.evidence == evidence;
    });
    if (!duplicate)
        escapes_.push_back({id, from, kind, std::move(evidence)});
}

void Graph::canonicalize() {
    std::vector<SymbolId> order(symbols_.size());
    for (SymbolId id = 0; id < order.size(); ++id)
        order[id] = id;
    std::ranges::sort(order, [&](SymbolId left, SymbolId right) {
        return symbols_[left].key < symbols_[right].key;
    });

    std::vector<SymbolId> remap(symbols_.size());
    std::vector<Symbol> sorted_symbols;
    sorted_symbols.reserve(symbols_.size());
    for (SymbolId id = 0; id < order.size(); ++id) {
        remap[order[id]] = id;
        sorted_symbols.push_back(std::move(symbols_[order[id]]));
    }
    symbols_ = std::move(sorted_symbols);
    key_to_id_.clear();
    for (SymbolId id = 0; id < symbols_.size(); ++id)
        key_to_id_.emplace(symbols_[id].key, id);

    for (auto& edge : edges_) {
        edge.from = remap[edge.from];
        edge.to = remap[edge.to];
    }
    for (auto& root : roots_)
        root.symbol = remap[root.symbol];
    for (auto& escape : escapes_) {
        escape.symbol = remap[escape.symbol];
        if (escape.from.has_value())
            escape.from = remap[*escape.from];
    }

    std::ranges::sort(edges_, [](const Edge& left, const Edge& right) {
        return std::tuple{left.from, left.to, left.kind, left.evidence.provider,
                          left.evidence.reason} < std::tuple{right.from, right.to, right.kind,
                                                             right.evidence.provider,
                                                             right.evidence.reason};
    });
    std::ranges::sort(roots_, [](const Root& left, const Root& right) {
        return std::tuple{left.symbol, left.kind, left.evidence.provider, left.evidence.reason} <
               std::tuple{right.symbol, right.kind, right.evidence.provider, right.evidence.reason};
    });
    std::ranges::sort(escapes_, [](const Escape& left, const Escape& right) {
        return std::tuple{left.symbol, left.from, left.kind, left.evidence.provider,
                          left.evidence.reason} < std::tuple{right.symbol, right.from, right.kind,
                                                             right.evidence.provider,
                                                             right.evidence.reason};
    });
}

std::optional<SymbolId> Graph::find_by_key(std::string_view key) const {
    const auto iterator = key_to_id_.find(std::string(key));
    if (iterator == key_to_id_.end())
        return std::nullopt;
    return iterator->second;
}

bool is_traversable(EdgeKind kind) {
    return kind == EdgeKind::DirectCall || kind == EdgeKind::Constructs ||
           kind == EdgeKind::VirtualDispatch;
}

bool has_indexed_body(SymbolScope scope) {
    return scope == SymbolScope::Reportable || scope == SymbolScope::Indexed;
}

bool is_reportable(SymbolScope scope) {
    return scope == SymbolScope::Reportable;
}

const SourceExtent& primary_source_extent(const Symbol& symbol) {
    return symbol.source.expansion.has_value() ? *symbol.source.expansion : symbol.source.spelling;
}

SymbolIdentity make_symbol_identity(std::string configuration_id, std::string usr,
                                    std::string linkage_name, std::string translation_unit,
                                    std::string fallback_anchor) {
    if (configuration_id.empty())
        throw std::invalid_argument("configuration identity cannot be empty");
    SymbolIdentity result{
        .configuration_id = std::move(configuration_id),
        .usr = std::move(usr),
        .linkage_name = std::move(linkage_name),
        .translation_unit = std::move(translation_unit),
        .fallback_anchor = std::move(fallback_anchor),
    };
    result.quality = !result.linkage_name.empty() || !result.usr.empty()
                         ? IdentityQuality::Stable
                         : IdentityQuality::Fallback;
    if (result.quality == IdentityQuality::Fallback && result.fallback_anchor.empty())
        throw std::invalid_argument("fallback symbol identity requires a source anchor");
    return result;
}

std::string stable_symbol_key(const SymbolIdentity& identity) {
    const auto append_component = [](std::string& output, std::string_view label,
                                     std::string_view value) {
        output += '|';
        output += label;
        output += '=';
        output += std::to_string(value.size());
        output += ':';
        output += value;
    };
    std::string result = "cxx-dead-symbol-v1";
    append_component(result, "configuration", identity.configuration_id);
    if (!identity.linkage_name.empty()) {
        append_component(result, "anchor", "linkage");
        append_component(result, "value", identity.linkage_name);
    } else if (!identity.usr.empty()) {
        append_component(result, "anchor", "usr");
        append_component(result, "value", identity.usr);
    } else {
        append_component(result, "anchor", "fallback");
        append_component(result, "value", identity.fallback_anchor);
    }
    append_component(result, "translation_unit", identity.translation_unit);
    return result;
}

void merge_graph(Graph& destination, const Graph& source) {
    std::vector<SymbolId> remap;
    remap.reserve(source.symbols().size());
    for (const auto& symbol : source.symbols())
        remap.push_back(destination.add_or_merge_symbol(symbol));
    for (const auto& edge : source.edges()) {
        destination.add_edge(remap[edge.from], remap[edge.to], edge.kind, edge.evidence);
    }
    for (const auto& root : source.roots())
        destination.add_root(remap[root.symbol], root.kind, root.evidence);
    for (const auto& escape : source.escapes()) {
        destination.add_escape(
            remap[escape.symbol], escape.kind, escape.evidence,
            escape.from.has_value() ? std::optional<SymbolId>{remap[*escape.from]} : std::nullopt);
    }
}

std::size_t graph_fact_bytes(const Graph& graph) {
    std::size_t result = 0;
    const auto point_bytes = [](const SourcePoint& point) {
        return point.file.string().size() + 4U * sizeof(std::size_t);
    };
    const auto extent_bytes = [&](const SourceExtent& extent) {
        return point_bytes(extent.location) + point_bytes(extent.begin) + point_bytes(extent.end);
    };
    for (const auto& symbol : graph.symbols()) {
        result += symbol.key.size() + symbol.identity.configuration_id.size() +
                  symbol.identity.usr.size() + symbol.identity.linkage_name.size() +
                  symbol.identity.translation_unit.size() + symbol.identity.fallback_anchor.size() +
                  symbol.name.size() + symbol.qualified_name.size() + symbol.class_name.size() +
                  symbol.signature.size() + extent_bytes(symbol.source.spelling) +
                  4U * sizeof(bool) + 2U * sizeof(int);
        if (symbol.source.expansion.has_value())
            result += extent_bytes(*symbol.source.expansion);
    }
    for (const auto& edge : graph.edges()) {
        result += 2U * sizeof(SymbolId) + sizeof(int) + edge.evidence.provider.size() +
                  edge.evidence.reason.size();
    }
    for (const auto& root : graph.roots()) {
        result += sizeof(SymbolId) + sizeof(int) + root.evidence.provider.size() +
                  root.evidence.reason.size();
    }
    for (const auto& escape : graph.escapes()) {
        result += 2U * sizeof(SymbolId) + sizeof(int) + escape.evidence.provider.size() +
                  escape.evidence.reason.size();
    }
    return result;
}

ReachabilityResult analyze_reachability(const Graph& graph) {
    const auto count = graph.symbols().size();
    std::vector<std::vector<SymbolId>> adjacency(count);
    for (const auto& edge : graph.edges()) {
        if (is_traversable(edge.kind) && has_indexed_body(graph.symbols()[edge.from].scope))
            adjacency[edge.from].push_back(edge.to);
    }

    ReachabilityResult result;
    result.reachable.assign(count, false);
    std::vector<SymbolId> stack;
    stack.reserve(graph.roots().size());
    for (const auto& root : graph.roots()) {
        if (!result.reachable[root.symbol]) {
            result.reachable[root.symbol] = true;
            stack.push_back(root.symbol);
        }
    }
    while (!stack.empty()) {
        const auto current = stack.back();
        stack.pop_back();
        for (const auto next : adjacency[current]) {
            if (!result.reachable[next]) {
                result.reachable[next] = true;
                stack.push_back(next);
            }
        }
    }

    const auto is_candidate = [&](SymbolId id) {
        const auto& symbol = graph.symbols()[id];
        return symbol.defined && is_reportable(symbol.scope) && !result.reachable[id] &&
               symbol.kind != SymbolKind::Synthetic;
    };

    std::vector<int> index(count, -1);
    std::vector<int> low_link(count, -1);
    std::vector<bool> on_stack(count, false);
    std::vector<SymbolId> tarjan_stack;
    int next_index = 0;

    std::function<void(SymbolId)> strong_connect = [&](SymbolId vertex) {
        index[vertex] = next_index;
        low_link[vertex] = next_index;
        ++next_index;
        tarjan_stack.push_back(vertex);
        on_stack[vertex] = true;

        for (const auto next : adjacency[vertex]) {
            if (!is_candidate(next))
                continue;
            if (index[next] == -1) {
                strong_connect(next);
                low_link[vertex] = std::min(low_link[vertex], low_link[next]);
            } else if (on_stack[next]) {
                low_link[vertex] = std::min(low_link[vertex], index[next]);
            }
        }

        if (low_link[vertex] == index[vertex]) {
            auto& component = result.unreachable_sccs.emplace_back();
            while (true) {
                const auto member = tarjan_stack.back();
                tarjan_stack.pop_back();
                on_stack[member] = false;
                component.push_back(member);
                if (member == vertex)
                    break;
            }
            std::ranges::sort(component, [&](SymbolId left, SymbolId right) {
                const auto& lhs = graph.symbols()[left];
                const auto& rhs = graph.symbols()[right];
                if (lhs.qualified_name != rhs.qualified_name)
                    return lhs.qualified_name < rhs.qualified_name;
                return lhs.signature < rhs.signature;
            });
        }
    };

    for (SymbolId id = 0; id < count; ++id) {
        if (is_candidate(id) && index[id] == -1)
            strong_connect(id);
    }
    std::ranges::sort(result.unreachable_sccs, [&](const auto& left, const auto& right) {
        const auto& lhs = graph.symbols()[left.front()];
        const auto& rhs = graph.symbols()[right.front()];
        const auto& lhs_location = primary_source_extent(lhs).location;
        const auto& rhs_location = primary_source_extent(rhs).location;
        if (lhs_location.file != rhs_location.file)
            return lhs_location.file.string() < rhs_location.file.string();
        return lhs_location.line < rhs_location.line;
    });
    return result;
}

std::string_view to_string(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Function:
        return "function";
    case SymbolKind::Method:
        return "method";
    case SymbolKind::Constructor:
        return "constructor";
    case SymbolKind::Destructor:
        return "destructor";
    case SymbolKind::Synthetic:
        return "synthetic";
    }
    return "unknown";
}

std::string_view to_string(SymbolScope scope) {
    switch (scope) {
    case SymbolScope::Reportable:
        return "reportable";
    case SymbolScope::Indexed:
        return "indexed";
    case SymbolScope::ExternalOpaque:
        return "external_opaque";
    case SymbolScope::Excluded:
        return "excluded";
    }
    return "unknown";
}

std::string_view to_string(EdgeKind kind) {
    switch (kind) {
    case EdgeKind::DirectCall:
        return "direct_call";
    case EdgeKind::Constructs:
        return "constructs";
    case EdgeKind::VirtualDispatch:
        return "virtual_dispatch";
    }
    return "unknown";
}

std::string_view to_string(RootKind kind) {
    switch (kind) {
    case RootKind::ApplicationEntryPoint:
        return "application_entry_point";
    case RootKind::GlobalInitializer:
        return "global_initializer";
    case RootKind::Manual:
        return "manual";
    }
    return "unknown";
}

std::string_view to_string(EscapeKind kind) {
    switch (kind) {
    case EscapeKind::AddressTaken:
        return "address_taken";
    }
    return "unknown";
}

std::string_view to_string(IdentityQuality quality) {
    switch (quality) {
    case IdentityQuality::Stable:
        return "stable";
    case IdentityQuality::Fallback:
        return "fallback";
    }
    return "unknown";
}

} // namespace cxx_dead
