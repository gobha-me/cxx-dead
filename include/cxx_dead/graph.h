#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cxx_dead {

using SymbolId = std::size_t;

enum class SymbolKind {
    Function,
    Method,
    Constructor,
    Destructor,
    Synthetic,
};

enum class EdgeKind {
    DirectCall,
    Constructs,
    VirtualDispatch,
    AddressTaken,
};

struct Symbol {
    std::string key;
    std::string name;
    std::string qualified_name;
    std::string class_name;
    std::string signature;
    std::filesystem::path file;
    std::size_t line{0};
    std::size_t end_line{0};
    SymbolKind kind{SymbolKind::Function};
    bool defined{false};
    bool project_owned{false};
    bool internal_linkage{false};
    bool is_virtual{false};
    bool address_taken{false};
};

struct Edge {
    SymbolId from{};
    SymbolId to{};
    EdgeKind kind{EdgeKind::DirectCall};
};

class Graph {
  public:
    SymbolId add_or_merge_symbol(Symbol symbol);
    void add_edge(SymbolId from, SymbolId to, EdgeKind kind);
    void add_root(SymbolId id, std::string reason);

    [[nodiscard]] std::optional<SymbolId> find_by_key(std::string_view key) const;
    [[nodiscard]] const std::vector<Symbol>& symbols() const {
        return symbols_;
    }
    [[nodiscard]] std::vector<Symbol>& symbols() {
        return symbols_;
    }
    [[nodiscard]] const std::vector<Edge>& edges() const {
        return edges_;
    }
    [[nodiscard]] const std::unordered_map<SymbolId, std::vector<std::string>>& roots() const {
        return roots_;
    }

  private:
    std::vector<Symbol> symbols_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, SymbolId> key_to_id_;
    std::unordered_map<SymbolId, std::vector<std::string>> roots_;
};

struct ReachabilityResult {
    std::vector<bool> reachable;
    std::vector<std::vector<SymbolId>> unreachable_sccs;
};

[[nodiscard]] bool is_traversable(EdgeKind kind);
[[nodiscard]] ReachabilityResult analyze_reachability(const Graph& graph);
[[nodiscard]] std::string_view to_string(SymbolKind kind);
[[nodiscard]] std::string_view to_string(EdgeKind kind);

} // namespace cxx_dead
