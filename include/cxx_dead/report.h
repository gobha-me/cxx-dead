#pragma once

#include "cxx_dead/indexer.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cxx_dead {

inline constexpr int report_schema_version = 11;

struct AnalysisMetadata {
    std::string mode{"application"};
    std::string configuration_id{"default"};
    std::string configuration;
    std::string target_id;
    std::string target_name;
    std::string target_kind;
    std::vector<std::string> closure_targets;
    RunDiagnostics run;
};

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

struct ProviderReachability {
    SymbolId symbol{};
    std::optional<SymbolId> from;
    Evidence evidence;
};

struct SuppressedFinding {
    Finding finding;
    std::vector<Evidence> suppressions;
};

enum class AggregateMemberDisposition {
    Actionable,
    Suppressed,
};

struct AggregateMember {
    SymbolId symbol{};
    AggregateMemberDisposition disposition{AggregateMemberDisposition::Actionable};
};

struct SourceLineEstimate {
    std::size_t estimated_loc{0};
    std::size_t unmeasured_symbols{0};
};

struct OwnershipSummary {
    std::string label;
    std::vector<AggregateMember> members;
    SourceLineEstimate lines;
};

struct UnreachableAggregate {
    std::size_t weak_component{};
    std::vector<std::size_t> sccs;
    std::vector<ReachabilityResult::CondensationEdge> edges;
    std::vector<AggregateMember> members;
    SourceLineEstimate lines;
    std::vector<OwnershipSummary> types;
    std::vector<OwnershipSummary> files;
    std::vector<OwnershipSummary> directories;
};

struct AnalysisReport {
    std::vector<Finding> findings;
    std::size_t reportable_symbols{0};
    std::size_t indexed_symbols{0};
    std::size_t external_opaque_symbols{0};
    std::size_t defined_symbols{0};
    std::size_t reachable_symbols{0};
    std::size_t structurally_reachable_symbols{0};
    std::size_t provider_reachable_symbols{0};
    std::size_t unreachable_symbols{0};
    std::size_t actionable_unreachable_symbols{0};
    std::size_t suppressed_symbols{0};
    std::size_t public_api_symbols{0};
    std::size_t internal_live_symbols{0};
    std::size_t internal_unreachable_symbols{0};
    std::vector<ProviderReachability> public_api;
    std::vector<ProviderReachability> provider_reachable;
    std::vector<SuppressedFinding> suppressed_findings;
    std::vector<UnreachableAggregate> unreachable_components;
};

[[nodiscard]] AnalysisReport build_report(const Graph& graph, const ReachabilityResult& result);
[[nodiscard]] std::string_view to_string(Classification classification);
[[nodiscard]] std::string_view to_string(FindingEvidenceKind kind);
void write_human_report(std::ostream& output, const Graph& graph,
                        const ReachabilityResult& reachability, const AnalysisReport& report,
                        const std::vector<std::string>& diagnostics,
                        const AnalysisMetadata& metadata = {});
void write_json_report(std::ostream& output, const Graph& graph,
                       const ReachabilityResult& reachability, const AnalysisReport& report,
                       const std::vector<std::string>& diagnostics,
                       const AnalysisMetadata& metadata = {});
void write_human_run_diagnostic(std::ostream& output, const IndexingError& error,
                                const AnalysisMetadata& metadata = {});
void write_json_run_diagnostic(std::ostream& output, const IndexingError& error,
                               const AnalysisMetadata& metadata = {});

} // namespace cxx_dead
