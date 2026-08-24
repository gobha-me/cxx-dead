#pragma once

#include "cxx_dead/graph.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cxx_dead {

enum class Classification {
    Dead,
    LikelyDead,
    PossiblyDead,
    DynamicallyReferenced,
};

enum class FindingEvidenceKind {
    NoReachablePath,
    InternalLinkage,
    VirtualDispatchUncertainty,
    Escape,
};

struct FindingEvidence {
    FindingEvidenceKind kind{FindingEvidenceKind::NoReachablePath};
    Evidence evidence;
    std::optional<EscapeKind> escape_kind;
    std::optional<SymbolId> from;
};

struct Finding {
    SymbolId symbol{};
    Classification classification{Classification::LikelyDead};
    double confidence{0.0};
    std::vector<FindingEvidence> evidence;
};

struct AnalysisReport {
    std::vector<Finding> findings;
    std::size_t defined_symbols{0};
    std::size_t reachable_symbols{0};
    std::size_t unreachable_symbols{0};
};

[[nodiscard]] AnalysisReport build_report(const Graph& graph, const ReachabilityResult& result);
[[nodiscard]] std::string_view to_string(Classification classification);
[[nodiscard]] std::string_view to_string(FindingEvidenceKind kind);
void write_human_report(std::ostream& output, const Graph& graph,
                        const ReachabilityResult& reachability, const AnalysisReport& report,
                        const std::vector<std::string>& diagnostics);
void write_json_report(std::ostream& output, const Graph& graph,
                       const ReachabilityResult& reachability, const AnalysisReport& report,
                       const std::vector<std::string>& diagnostics);

} // namespace cxx_dead
