#include "cxx_dead/indexer.h"

#include <stdexcept>
#include <utility>

namespace cxx_dead {

LibToolingIndexer::LibToolingIndexer(IndexOptions options) : options_(std::move(options)) {}

IndexResult LibToolingIndexer::index(const std::vector<CompileCommand>&) const {
    throw std::runtime_error(
        "LibTooling frontend is unavailable; rebuild with CXX_DEAD_ENABLE_LIBTOOLING=ON");
}

bool libtooling_available() {
    return false;
}

} // namespace cxx_dead
