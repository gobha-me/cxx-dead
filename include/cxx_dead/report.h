#pragma once

#include "cxx_dead/graph.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace cxx_dead {

struct Finding {
    SymbolId symbol{};
    std::string classification;
    double confidence{0.0};
    std::string reason;
};

struct AnalysisReport {
    std::vector<Finding> findings;
    std::size_t defined_symbols{0};
    std::size_t reachable_symbols{0};
    std::size_t unreachable_symbols{0};
};

[[nodiscard]] AnalysisReport build_report(const Graph& graph, const ReachabilityResult& result);
void write_human_report(std::ostream& output, const Graph& graph,
                        const ReachabilityResult& reachability, const AnalysisReport& report,
                        const std::vector<std::string>& diagnostics);
void write_json_report(std::ostream& output, const Graph& graph,
                       const ReachabilityResult& reachability, const AnalysisReport& report,
                       const std::vector<std::string>& diagnostics);

} // namespace cxx_dead
