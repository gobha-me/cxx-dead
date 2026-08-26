#pragma once

#include "cxx_dead/indexer.h"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace cxx_dead {

inline constexpr int graph_artifact_schema_version = 1;
inline constexpr int symbol_identity_schema_version = 1;

struct GraphArtifactMetadata {
    std::string configuration_id{"default"};
    IndexFrontend frontend{IndexFrontend::AstJson};
    std::size_t translation_units{0};
};

void write_graph_artifact(std::ostream& output, const Graph& graph,
                          const GraphArtifactMetadata& metadata,
                          const std::vector<std::string>& diagnostics);

} // namespace cxx_dead
