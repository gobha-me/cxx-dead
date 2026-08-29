#pragma once

#include <chrono>
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

enum class SymbolScope {
    Reportable,
    Indexed,
    ExternalOpaque,
    Excluded,
};

enum class EdgeKind {
    DirectCall,
    Constructs,
    VirtualDispatch,
    CallbackRegistration,
    Provider,
};

enum class RootKind {
    ApplicationEntryPoint,
    GlobalInitializer,
    Manual,
    CallbackRegistration,
    Provider,
    PublicApi,
};

enum class EscapeKind {
    AddressTaken,
    CallableObject,
    Provider,
};

enum class IdentityQuality {
    Stable,
    Fallback,
};

struct SymbolIdentity {
    std::string configuration_id{"default"};
    std::string usr;
    std::string linkage_name;
    std::string translation_unit;
    std::string fallback_anchor;
    IdentityQuality quality{IdentityQuality::Fallback};

    bool operator==(const SymbolIdentity&) const = default;
};

struct Evidence {
    std::string provider;
    std::string reason;

    bool operator==(const Evidence&) const = default;
};

struct SourcePoint {
    std::filesystem::path file;
    std::size_t line{0};
    std::size_t column{0};
    std::size_t offset{0};
    std::size_t token_length{0};
};

struct SourceExtent {
    SourcePoint location;
    SourcePoint begin;
    SourcePoint end;
};

struct SymbolSource {
    SourceExtent spelling;
    std::optional<SourceExtent> expansion;
};

struct Symbol {
    std::string key;
    SymbolIdentity identity;
    std::string name;
    std::string qualified_name;
    std::string class_name;
    std::string signature;
    SymbolSource source;
    SymbolKind kind{SymbolKind::Function};
    SymbolScope scope{SymbolScope::ExternalOpaque};
    bool defined{false};
    bool internal_linkage{false};
    bool is_virtual{false};
};

struct Edge {
    SymbolId from{};
    SymbolId to{};
    EdgeKind kind{EdgeKind::DirectCall};
    Evidence evidence;
};

struct Root {
    SymbolId symbol{};
    RootKind kind{RootKind::ApplicationEntryPoint};
    Evidence evidence;
};

struct Escape {
    SymbolId symbol{};
    std::optional<SymbolId> from;
    EscapeKind kind{EscapeKind::AddressTaken};
    Evidence evidence;
};

struct Suppression {
    SymbolId symbol{};
    Evidence evidence;
};

class Graph {
  public:
    SymbolId add_or_merge_symbol(Symbol symbol);
    void add_edge(SymbolId from, SymbolId to, EdgeKind kind, Evidence evidence);
    void add_root(SymbolId id, RootKind kind, Evidence evidence);
    void add_escape(SymbolId id, EscapeKind kind, Evidence evidence,
                    std::optional<SymbolId> from = std::nullopt);
    void add_suppression(SymbolId id, Evidence evidence);
    void canonicalize();

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
    [[nodiscard]] const std::vector<Root>& roots() const {
        return roots_;
    }
    [[nodiscard]] const std::vector<Escape>& escapes() const {
        return escapes_;
    }
    [[nodiscard]] const std::vector<Suppression>& suppressions() const {
        return suppressions_;
    }

  private:
    std::vector<Symbol> symbols_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, SymbolId> key_to_id_;
    std::vector<Root> roots_;
    std::vector<Escape> escapes_;
    std::vector<Suppression> suppressions_;
};

struct ReachabilityResult {
    std::vector<bool> reachable;
    std::vector<bool> structurally_reachable;
    std::vector<bool> provider_reachable;
    std::vector<std::vector<SymbolId>> unreachable_sccs;
    struct CondensationEdge {
        std::size_t from_scc{};
        std::size_t to_scc{};

        bool operator==(const CondensationEdge&) const = default;
    };
    std::vector<CondensationEdge> unreachable_condensation_edges;
    std::vector<std::vector<std::size_t>> unreachable_weak_components;
};

struct ReachabilityMetrics {
    std::chrono::milliseconds traversal_time{0};
    std::chrono::milliseconds scc_time{0};
};

[[nodiscard]] bool is_traversable(EdgeKind kind);
[[nodiscard]] bool is_provider(EdgeKind kind);
[[nodiscard]] bool is_provider(RootKind kind);
[[nodiscard]] bool is_public_api(RootKind kind);
[[nodiscard]] bool has_indexed_body(SymbolScope scope);
[[nodiscard]] bool is_reportable(SymbolScope scope);
[[nodiscard]] const SourceExtent& primary_source_extent(const Symbol& symbol);
[[nodiscard]] SymbolIdentity make_symbol_identity(std::string configuration_id, std::string usr,
                                                  std::string linkage_name,
                                                  std::string translation_unit,
                                                  std::string fallback_anchor);
[[nodiscard]] std::string stable_symbol_key(const SymbolIdentity& identity);
void merge_graph(Graph& destination, const Graph& source);
[[nodiscard]] std::size_t graph_fact_bytes(const Graph& graph);
[[nodiscard]] ReachabilityResult analyze_reachability(const Graph& graph);
[[nodiscard]] ReachabilityResult analyze_reachability(const Graph& graph,
                                                      ReachabilityMetrics& metrics);
[[nodiscard]] std::string_view to_string(SymbolKind kind);
[[nodiscard]] std::string_view to_string(SymbolScope scope);
[[nodiscard]] std::string_view to_string(EdgeKind kind);
[[nodiscard]] std::string_view to_string(RootKind kind);
[[nodiscard]] std::string_view to_string(EscapeKind kind);
[[nodiscard]] std::string_view to_string(IdentityQuality quality);

} // namespace cxx_dead
