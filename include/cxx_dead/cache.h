#pragma once

#include "cxx_dead/indexer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cxx_dead {

inline constexpr int translation_unit_cache_schema_version = 1;

struct CachedRecordHierarchy {
    std::string name;
    std::vector<std::string> bases;
};

struct CacheDependency {
    std::filesystem::path path;
    std::string digest;
};

struct TranslationUnitCacheEntry {
    Graph graph;
    std::vector<std::string> diagnostics;
    std::vector<bool> registration_rule_matches;
    std::vector<CachedRecordHierarchy> record_hierarchy;
    std::vector<CacheDependency> dependencies;
    std::size_t ast_bytes{0};
    std::size_t fact_bytes{0};
};

struct CacheLookupResult {
    std::optional<TranslationUnitCacheEntry> entry;
    std::vector<std::string> warnings;
    std::size_t bytes_read{0};
};

class StagedCacheWrite {
  public:
    StagedCacheWrite() = default;
    StagedCacheWrite(std::filesystem::path temporary, std::filesystem::path destination,
                     std::size_t bytes);
    StagedCacheWrite(const StagedCacheWrite&) = delete;
    StagedCacheWrite& operator=(const StagedCacheWrite&) = delete;
    StagedCacheWrite(StagedCacheWrite&& other) noexcept;
    StagedCacheWrite& operator=(StagedCacheWrite&& other) noexcept;
    ~StagedCacheWrite();

    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_;
    }
    void commit(std::vector<std::string>& warnings);

  private:
    void discard() noexcept;

    std::filesystem::path temporary_;
    std::filesystem::path destination_;
    std::size_t bytes_{0};
};

[[nodiscard]] std::string translation_unit_cache_key(const CompileCommand& command,
                                                     const IndexOptions& options,
                                                     IndexFrontend frontend);
[[nodiscard]] CacheLookupResult
load_translation_unit_cache(const std::filesystem::path& directory, std::string_view key,
                            const std::filesystem::path& required_source);
[[nodiscard]] std::optional<StagedCacheWrite>
stage_translation_unit_cache(const std::filesystem::path& directory, std::string_view key,
                             const TranslationUnitCacheEntry& entry,
                             std::vector<std::string>& warnings);
[[nodiscard]] std::vector<CacheDependency>
hash_cache_dependencies(std::vector<std::filesystem::path> paths);
[[nodiscard]] std::vector<std::filesystem::path>
parse_make_dependencies(const std::filesystem::path& depfile,
                        const std::filesystem::path& working_directory);
[[nodiscard]] std::filesystem::path cache_temporary_path(std::string_view suffix);

} // namespace cxx_dead
