#pragma once

#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cxx_dead {

enum class IndexFrontend {
    AstJson,
    LibTooling,
};

struct IndexOptions {
    std::filesystem::path project_root;
    std::string configuration_id{"default"};
    std::vector<std::filesystem::path> report_paths;
    std::vector<std::filesystem::path> excluded_paths;
    std::string clang_executable{"clang++"};
    std::string ast_filter;
    std::vector<std::string> manual_roots;
    bool verbose{false};
};

struct IndexResult {
    Graph graph;
    std::vector<std::string> diagnostics;
    IndexFrontend frontend{IndexFrontend::AstJson};
    std::size_t translation_units{0};
    std::size_t ast_bytes{0};
    std::size_t fact_bytes{0};
};

class ClangAstIndexer {
  public:
    explicit ClangAstIndexer(IndexOptions options);

    [[nodiscard]] IndexResult index(const std::vector<CompileCommand>& commands) const;

  private:
    IndexOptions options_;
};

class LibToolingIndexer {
  public:
    explicit LibToolingIndexer(IndexOptions options);

    [[nodiscard]] IndexResult index(const std::vector<CompileCommand>& commands) const;

  private:
    IndexOptions options_;
};

[[nodiscard]] bool libtooling_available();
[[nodiscard]] std::string_view to_string(IndexFrontend frontend);

} // namespace cxx_dead
