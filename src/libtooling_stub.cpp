#include "cxx_dead/indexer.h"

#include <stdexcept>
#include <utility>

namespace cxx_dead {

LibToolingIndexer::LibToolingIndexer(IndexOptions options) : options_(std::move(options)) {}

IndexResult LibToolingIndexer::index(const std::vector<CompileCommand>& commands) const {
    RunDiagnostics diagnostics{
        .state = RunState::Unsupported,
        .frontend = IndexFrontend::LibTooling,
        .partial_graph_discarded = false,
        .translation_units = {},
    };
    for (const auto& command : commands) {
        diagnostics.translation_units.push_back({
            .file = command.file,
            .status = TranslationUnitStatus::Unsupported,
            .stage = "frontend",
            .message = "LibTooling frontend is not available in this build",
            .exit_code = std::nullopt,
            .signal = std::nullopt,
        });
    }
    throw IndexingError(
        "LibTooling frontend is unavailable; rebuild with CXX_DEAD_ENABLE_LIBTOOLING=ON",
        std::move(diagnostics));
}

bool libtooling_available() {
    return false;
}

} // namespace cxx_dead
