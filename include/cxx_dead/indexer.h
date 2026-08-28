#pragma once

#include "cxx_dead/compile_database.h"
#include "cxx_dead/graph.h"
#include "cxx_dead/provider.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cxx_dead {

enum class IndexFrontend {
    AstJson,
    LibTooling,
};

enum class RunState {
    Complete,
    Incomplete,
    Unsupported,
};

enum class TranslationUnitStatus {
    Indexed,
    Failed,
    Skipped,
    TimedOut,
    Unsupported,
    Cancelled,
};

struct TranslationUnitDiagnostic {
    std::filesystem::path file;
    TranslationUnitStatus status{TranslationUnitStatus::Indexed};
    std::string stage;
    std::string message;
    std::optional<int> exit_code;
    std::optional<int> signal;
};

struct RunDiagnostics {
    RunState state{RunState::Complete};
    IndexFrontend frontend{IndexFrontend::AstJson};
    bool partial_graph_discarded{false};
    std::vector<TranslationUnitDiagnostic> translation_units;
};

class IndexingError : public std::runtime_error {
  public:
    IndexingError(std::string message, RunDiagnostics diagnostics)
        : std::runtime_error(std::move(message)), diagnostics_(std::move(diagnostics)) {}

    [[nodiscard]] const RunDiagnostics& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    RunDiagnostics diagnostics_;
};

struct IndexOptions {
    std::filesystem::path project_root;
    std::string configuration_id{"default"};
    std::vector<std::filesystem::path> report_paths;
    std::vector<std::filesystem::path> excluded_paths;
    std::string clang_executable{"clang++"};
    std::string ast_filter;
    std::vector<std::string> manual_roots;
    std::vector<CallbackRegistrationRule> callback_registration_rules;
    std::vector<ProviderPolicy> provider_policies;
    std::vector<std::filesystem::path> selected_target_sources;
    std::vector<std::filesystem::path> public_headers;
    std::chrono::milliseconds translation_unit_timeout{0};
    std::chrono::milliseconds index_timeout{0};
    std::size_t max_ast_bytes{0};
    std::optional<std::filesystem::path> cache_directory;
    std::function<bool()> cancellation_requested;
    bool verbose{false};
    bool infer_shared_library_exports{false};
    bool require_library_api_policy{false};
};

struct IndexMetrics {
    std::size_t cache_hits{0};
    std::size_t cache_misses{0};
    std::size_t cache_bytes_read{0};
    std::size_t cache_bytes_written{0};
    std::chrono::milliseconds cache_validation_time{0};
    std::chrono::milliseconds indexing_time{0};
    std::chrono::milliseconds merge_time{0};
};

struct IndexResult {
    Graph graph;
    std::vector<std::string> diagnostics;
    IndexFrontend frontend{IndexFrontend::AstJson};
    std::size_t translation_units{0};
    std::size_t ast_bytes{0};
    std::size_t fact_bytes{0};
    IndexMetrics metrics;
    std::vector<std::string> cache_warnings;
    std::vector<TranslationUnitDiagnostic> translation_unit_diagnostics;
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
[[nodiscard]] std::string_view to_string(RunState state);
[[nodiscard]] std::string_view to_string(TranslationUnitStatus status);

} // namespace cxx_dead
