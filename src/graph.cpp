#include "cxx_dead/graph.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace cxx_dead {

namespace {

void merge_symbol(Symbol& target, Symbol source) {
    const bool target_was_project_owned = target.project_owned;
    const bool target_was_defined = target.defined;
    target.defined = target.defined || source.defined;
    target.project_owned = target.project_owned || source.project_owned;
    target.internal_linkage = target.internal_linkage || source.internal_linkage;
    target.is_virtual = target.is_virtual || source.is_virtual;
    if ((target.file.empty() || (!target_was_project_owned && source.project_owned) ||
         (!target_was_defined && source.defined)) &&
        !source.file.empty()) {
        target.file = std::move(source.file);
        target.line = source.line;
        target.end_line = source.end_line;
    }
    if (target.qualified_name.empty())
        target.qualified_name = std::move(source.qualified_name);
    if (target.class_name.empty())
        target.class_name = std::move(source.class_name);
    if (target.signature.empty())
        target.signature = std::move(source.signature);
}

} // namespace

SymbolId Graph::add_or_merge_symbol(Symbol symbol) {
    if (symbol.key.empty()) {
        throw std::invalid_argument("symbol key cannot be empty");
    }
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

ReachabilityResult analyze_reachability(const Graph& graph) {
    const auto count = graph.symbols().size();
    std::vector<std::vector<SymbolId>> adjacency(count);
    for (const auto& edge : graph.edges()) {
        if (is_traversable(edge.kind))
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
        return symbol.defined && symbol.project_owned && !result.reachable[id] &&
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
                return graph.symbols()[left].qualified_name < graph.symbols()[right].qualified_name;
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
        if (lhs.file != rhs.file)
            return lhs.file.string() < rhs.file.string();
        return lhs.line < rhs.line;
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

} // namespace cxx_dead
