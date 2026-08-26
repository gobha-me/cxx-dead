#pragma once

#include "cxx_dead/compile_database.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cxx_dead {

enum class BuildTargetKind {
    Executable,
    StaticLibrary,
    SharedLibrary,
    ModuleLibrary,
    ObjectLibrary,
    InterfaceLibrary,
    Utility,
};

struct BuildTarget {
    std::string id;
    std::string name;
    BuildTargetKind kind{BuildTargetKind::Utility};
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> dependencies;
    std::vector<std::filesystem::path> artifacts;
};

struct BuildConfiguration {
    std::string name;
    std::vector<BuildTarget> targets;
};

struct BuildModel {
    std::filesystem::path source_root;
    std::filesystem::path build_root;
    std::vector<BuildConfiguration> configurations;
};

struct TargetAnalysisContext {
    std::string configuration;
    std::string target_id;
    std::string target_name;
    BuildTargetKind target_kind{BuildTargetKind::Executable};
    std::vector<std::string> closure_targets;
};

struct TargetSelection {
    TargetAnalysisContext context;
    std::vector<CompileCommand> commands;
    std::vector<std::string> diagnostics;
};

[[nodiscard]] BuildModel load_cmake_file_api(const std::filesystem::path& build_directory);
[[nodiscard]] BuildModel load_target_manifest(const std::filesystem::path& manifest);
[[nodiscard]] TargetSelection select_target_commands(const BuildModel& model,
                                                     std::string_view configuration,
                                                     std::string_view target,
                                                     const std::vector<CompileCommand>& commands);
[[nodiscard]] std::string_view to_string(BuildTargetKind kind);

} // namespace cxx_dead
