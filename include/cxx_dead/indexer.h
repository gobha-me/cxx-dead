#pragma once

#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cxx_dead {

struct IndexOptions {
    std::filesystem::path project_root;
    std::vector<std::filesystem::path> excluded_paths;
    std::string clang_executable{"clang++"};
    std::string ast_filter;
    std::vector<std::string> manual_roots;
    bool verbose{false};
};

struct IndexResult {
    Graph graph;
    std::vector<std::string> diagnostics;
    std::size_t translation_units{0};
    std::size_t ast_bytes{0};
};

class ClangAstIndexer {
  public:
    explicit ClangAstIndexer(IndexOptions options);

    [[nodiscard]] IndexResult index(const std::vector<CompileCommand>& commands) const;

  private:
    IndexOptions options_;
};

} // namespace cxx_dead
