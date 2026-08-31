#pragma once

#include "cxx_dead/artifact.h"
#include "cxx_dead/report.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cxx_dead {

inline constexpr int differential_report_schema_version = 2;
inline constexpr int differential_policy_schema_version = 2;

enum class ChangeKind {
    NewSymbol,
    NewlyUnreachable,
    Removed,
    BecameReachable,
};

struct DifferentialPolicy {
    std::filesystem::path source;
    std::vector<ChangeKind> changes{ChangeKind::NewSymbol, ChangeKind::NewlyUnreachable};
    std::vector<Classification> classifications{
        Classification::Dead,
        Classification::LikelyDead,
        Classification::PossiblyDead,
        Classification::DynamicallyReferenced,
    };
    std::vector<std::string> targets;
};

struct DifferentialEvidence {
    FindingEvidenceKind kind{FindingEvidenceKind::NoReachablePath};
    Evidence evidence;
    std::optional<EscapeKind> escape_kind;
    std::string from_symbol;
};

struct DifferentialSymbolState {
    bool present{false};
    bool reachable{false};
    bool suppressed{false};
    std::optional<Classification> classification;
    std::vector<DifferentialEvidence> evidence;
    std::vector<Evidence> suppressions;
};

struct DifferentialChange {
    ChangeKind kind{ChangeKind::NewSymbol};
    Symbol symbol;
    DifferentialSymbolState baseline;
    DifferentialSymbolState current;
    bool policy_match{false};
};

struct DifferentialReport {
    GraphArtifactMetadata baseline;
    GraphArtifactMetadata current;
    std::vector<DifferentialChange> changes;
    std::size_t new_symbols{0};
    std::size_t newly_unreachable{0};
    std::size_t removed{0};
    std::size_t became_reachable{0};
    std::size_t policy_matches{0};
};

[[nodiscard]] DifferentialPolicy load_differential_policy(const std::filesystem::path& path);
[[nodiscard]] DifferentialReport
build_differential_report(const GraphArtifact& baseline, const Graph& current_graph,
                          const ReachabilityResult& current_reachability,
                          const AnalysisReport& current_report,
                          const GraphArtifactMetadata& current_metadata,
                          const std::optional<DifferentialPolicy>& policy = std::nullopt);
[[nodiscard]] std::string_view to_string(ChangeKind kind);
void write_human_differential_report(std::ostream& output, const DifferentialReport& report);
void write_json_differential_report(std::ostream& output, const DifferentialReport& report);
void write_sarif_differential_report(std::ostream& output, const DifferentialReport& report,
                                     const std::filesystem::path& project_root,
                                     std::string_view tool_version);

} // namespace cxx_dead
