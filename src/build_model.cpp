#include "cxx_dead/build_model.h"

#include "cxx_dead/json.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cxx_dead {
namespace {

using json::Value;

Value read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open build metadata: " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    try {
        return json::parse(contents.str());
    } catch (const std::exception& error) {
        throw std::runtime_error("invalid build metadata " + path.string() + ": " + error.what());
    }
}

const Value& require_member(const Value& object, std::string_view key, std::string_view context) {
    const auto* value = object.find(key);
    if (value == nullptr)
        throw std::runtime_error(std::string(context) + " is missing '" + std::string(key) + "'");
    return *value;
}

const Value::Array& require_array(const Value& object, std::string_view key,
                                  std::string_view context) {
    const auto& value = require_member(object, key, context);
    if (!value.is_array())
        throw std::runtime_error(std::string(context) + " '" + std::string(key) +
                                 "' must be an array");
    return value.as_array();
}

std::string require_string(const Value& object, std::string_view key, std::string_view context) {
    const auto& value = require_member(object, key, context);
    if (!value.is_string() || value.as_string().empty())
        throw std::runtime_error(std::string(context) + " '" + std::string(key) +
                                 "' must be a non-empty string");
    return value.as_string();
}

std::size_t require_index(const Value& value, std::string_view context) {
    if (!value.is_number() || value.as_number() < 0.0 ||
        std::floor(value.as_number()) != value.as_number()) {
        throw std::runtime_error(std::string(context) + " must be a non-negative integer");
    }
    return static_cast<std::size_t>(value.as_number());
}

std::filesystem::path normalized_path(const std::filesystem::path& path,
                                      const std::filesystem::path& base) {
    const auto joined = path.is_absolute() ? path : base / path;
    return std::filesystem::absolute(joined).lexically_normal();
}

std::filesystem::path referenced_file(const std::filesystem::path& directory,
                                      std::string_view name) {
    const std::filesystem::path relative{name};
    if (relative.empty() || relative.is_absolute())
        throw std::runtime_error("CMake File API reply contains an invalid jsonFile path");
    const auto result = (directory / relative).lexically_normal();
    const auto relation = result.lexically_relative(directory);
    if (relation.empty() || relation.is_absolute() || *relation.begin() == "..")
        throw std::runtime_error("CMake File API jsonFile escapes the reply directory");
    return result;
}

BuildTargetKind parse_target_kind(std::string_view kind) {
    if (kind == "EXECUTABLE" || kind == "executable")
        return BuildTargetKind::Executable;
    if (kind == "STATIC_LIBRARY" || kind == "static_library")
        return BuildTargetKind::StaticLibrary;
    if (kind == "SHARED_LIBRARY" || kind == "shared_library")
        return BuildTargetKind::SharedLibrary;
    if (kind == "MODULE_LIBRARY" || kind == "module_library")
        return BuildTargetKind::ModuleLibrary;
    if (kind == "OBJECT_LIBRARY" || kind == "object_library")
        return BuildTargetKind::ObjectLibrary;
    if (kind == "INTERFACE_LIBRARY" || kind == "interface_library")
        return BuildTargetKind::InterfaceLibrary;
    if (kind == "UTILITY" || kind == "utility")
        return BuildTargetKind::Utility;
    throw std::runtime_error("unsupported build target type: " + std::string(kind));
}

bool dependency_is_linked(const Value& target, const Value& dependency) {
    const auto* backtrace = dependency.find("backtrace");
    const auto* graph = target.find("backtraceGraph");
    if (backtrace == nullptr || graph == nullptr || !graph->is_object())
        return true;
    const auto* nodes = graph->find("nodes");
    const auto* commands = graph->find("commands");
    if (!backtrace->is_number() || nodes == nullptr || !nodes->is_array() || commands == nullptr ||
        !commands->is_array()) {
        return true;
    }
    const auto node_index = require_index(*backtrace, "dependency backtrace");
    if (node_index >= nodes->as_array().size())
        return true;
    const auto* command = nodes->as_array()[node_index].find("command");
    if (command == nullptr || !command->is_number())
        return true;
    const auto command_index = require_index(*command, "backtrace command");
    if (command_index >= commands->as_array().size() ||
        !commands->as_array()[command_index].is_string()) {
        return true;
    }
    return commands->as_array()[command_index].as_string() != "add_dependencies";
}

BuildTarget parse_cmake_target(const std::filesystem::path& target_file,
                               const std::filesystem::path& source_root,
                               const std::filesystem::path& build_root) {
    const auto document = read_json(target_file);
    if (!document.is_object())
        throw std::runtime_error("CMake target reply must be an object: " + target_file.string());
    BuildTarget result;
    result.id = require_string(document, "id", "CMake target");
    result.name = require_string(document, "name", "CMake target");
    result.kind = parse_target_kind(require_string(document, "type", "CMake target"));

    std::set<std::size_t> cxx_compile_groups;
    if (const auto* groups = document.find("compileGroups"); groups != nullptr) {
        if (!groups->is_array())
            throw std::runtime_error("CMake target compileGroups must be an array");
        for (std::size_t index = 0; index < groups->as_array().size(); ++index) {
            const auto& group = groups->as_array()[index];
            if (group.string_or("language") == "CXX")
                cxx_compile_groups.insert(index);
        }
    }
    if (const auto* sources = document.find("sources"); sources != nullptr) {
        if (!sources->is_array())
            throw std::runtime_error("CMake target sources must be an array");
        for (const auto& source : sources->as_array()) {
            const auto* group = source.find("compileGroupIndex");
            if (group == nullptr || !group->is_number())
                continue;
            if (!cxx_compile_groups.contains(require_index(*group, "compileGroupIndex")))
                continue;
            result.sources.push_back(normalized_path(
                require_string(source, "path", "CMake target source"), source_root));
        }
    }
    if (const auto* dependencies = document.find("dependencies"); dependencies != nullptr) {
        if (!dependencies->is_array())
            throw std::runtime_error("CMake target dependencies must be an array");
        for (const auto& dependency : dependencies->as_array()) {
            if (dependency_is_linked(document, dependency))
                result.dependencies.push_back(
                    require_string(dependency, "id", "CMake target dependency"));
        }
    }
    if (const auto* artifacts = document.find("artifacts"); artifacts != nullptr) {
        if (!artifacts->is_array())
            throw std::runtime_error("CMake target artifacts must be an array");
        for (const auto& artifact : artifacts->as_array()) {
            result.artifacts.push_back(normalized_path(
                require_string(artifact, "path", "CMake target artifact"), build_root));
        }
    }
    std::ranges::sort(result.sources);
    result.sources.erase(std::ranges::unique(result.sources).begin(), result.sources.end());
    std::ranges::sort(result.dependencies);
    result.dependencies.erase(std::ranges::unique(result.dependencies).begin(),
                              result.dependencies.end());
    return result;
}

std::filesystem::path newest_index(const std::filesystem::path& reply_directory) {
    std::optional<std::filesystem::path> selected;
    std::filesystem::file_time_type selected_time{};
    for (const auto& entry : std::filesystem::directory_iterator(reply_directory)) {
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file() || !name.starts_with("index-") ||
            entry.path().extension() != ".json") {
            continue;
        }
        const auto modified = entry.last_write_time();
        if (!selected.has_value() || modified > selected_time ||
            (modified == selected_time && entry.path().filename() > selected->filename())) {
            selected = entry.path();
            selected_time = modified;
        }
    }
    if (!selected.has_value())
        throw std::runtime_error("no CMake File API index reply found in " +
                                 reply_directory.string());
    return *selected;
}

const BuildConfiguration& select_configuration(const BuildModel& model,
                                               std::string_view requested) {
    if (!requested.empty()) {
        const auto iterator =
            std::ranges::find(model.configurations, requested, &BuildConfiguration::name);
        if (iterator == model.configurations.end())
            throw std::runtime_error("build configuration not found: " + std::string(requested));
        return *iterator;
    }
    if (model.configurations.size() != 1U)
        throw std::runtime_error("build metadata has multiple configurations; select one with "
                                 "--configuration");
    return model.configurations.front();
}

const BuildTarget& select_target(const BuildConfiguration& configuration,
                                 std::string_view requested) {
    if (!requested.empty()) {
        const BuildTarget* match = nullptr;
        for (const auto& target : configuration.targets) {
            if (target.name != requested && target.id != requested)
                continue;
            if (match != nullptr)
                throw std::runtime_error("target name is ambiguous; select by target id: " +
                                         std::string(requested));
            match = &target;
        }
        if (match == nullptr)
            throw std::runtime_error("build target not found: " + std::string(requested));
        return *match;
    }
    const BuildTarget* executable = nullptr;
    for (const auto& target : configuration.targets) {
        if (target.kind != BuildTargetKind::Executable)
            continue;
        if (executable != nullptr)
            throw std::runtime_error("build metadata has multiple executable targets; select one "
                                     "with --target");
        executable = &target;
    }
    if (executable == nullptr)
        throw std::runtime_error("build metadata has no executable target; select a library with "
                                 "--target and provide an explicit --root");
    return *executable;
}

bool command_matches_target(const CompileCommand& command, const BuildTarget& target) {
    if (command.output.empty())
        return false;
    const auto marker = "/CMakeFiles/" + target.name + ".dir/";
    const auto output = "/" + command.output.generic_string();
    return output.find(marker) != std::string::npos;
}

bool command_matches_configuration(const CompileCommand& command, const BuildTarget& target,
                                   std::string_view configuration) {
    if (configuration.empty() || command.output.empty())
        return false;
    const auto marker = "/CMakeFiles/" + target.name + ".dir/";
    const auto output = "/" + command.output.generic_string();
    const auto marker_position = output.find(marker);
    if (marker_position == std::string::npos)
        return false;
    const auto suffix = std::string_view(output).substr(marker_position + marker.size());
    return suffix.starts_with(std::string(configuration) + "/");
}

bool equivalent_commands(const CompileCommand* left, const CompileCommand* right) {
    return left->directory == right->directory && left->file == right->file &&
           left->arguments == right->arguments;
}

} // namespace

BuildModel load_cmake_file_api(const std::filesystem::path& build_directory) {
    const auto build_root = std::filesystem::absolute(build_directory).lexically_normal();
    const auto reply_directory = build_root / ".cmake" / "api" / "v1" / "reply";
    if (!std::filesystem::is_directory(reply_directory)) {
        throw std::runtime_error("CMake File API reply directory does not exist: " +
                                 reply_directory.string());
    }
    const auto index_file = newest_index(reply_directory);
    const auto index = read_json(index_file);
    const auto& objects = require_array(index, "objects", "CMake File API index");
    const Value* codemodel_reference = nullptr;
    for (const auto& object : objects) {
        if (object.string_or("kind") != "codemodel")
            continue;
        const auto* version = object.find("version");
        const auto* major = version != nullptr ? version->find("major") : nullptr;
        if (major != nullptr && major->is_number() && major->as_number() == 2.0) {
            codemodel_reference = &object;
            break;
        }
    }
    if (codemodel_reference == nullptr)
        throw std::runtime_error("CMake File API index has no codemodel-v2 reply");
    const auto codemodel_file =
        referenced_file(reply_directory, require_string(*codemodel_reference, "jsonFile",
                                                        "CMake codemodel reference"));
    const auto codemodel = read_json(codemodel_file);
    const auto& paths = require_member(codemodel, "paths", "CMake codemodel");
    BuildModel result;
    result.source_root = normalized_path(require_string(paths, "source", "CMake codemodel paths"),
                                         codemodel_file.parent_path());
    result.build_root = normalized_path(require_string(paths, "build", "CMake codemodel paths"),
                                        codemodel_file.parent_path());
    for (const auto& configuration_value :
         require_array(codemodel, "configurations", "CMake codemodel")) {
        BuildConfiguration configuration;
        configuration.name = configuration_value.string_or("name");
        for (const auto& target_reference :
             require_array(configuration_value, "targets", "CMake configuration")) {
            const auto target_file =
                referenced_file(reply_directory, require_string(target_reference, "jsonFile",
                                                                "CMake target reference"));
            auto target = parse_cmake_target(target_file, result.source_root, result.build_root);
            if (target.id != require_string(target_reference, "id", "CMake target reference") ||
                target.name != require_string(target_reference, "name", "CMake target reference")) {
                throw std::runtime_error("CMake target reference does not match target reply: " +
                                         target_file.string());
            }
            configuration.targets.push_back(std::move(target));
        }
        result.configurations.push_back(std::move(configuration));
    }
    if (result.configurations.empty())
        throw std::runtime_error("CMake codemodel has no configurations");
    return result;
}

BuildModel load_target_manifest(const std::filesystem::path& manifest) {
    const auto manifest_path = std::filesystem::absolute(manifest).lexically_normal();
    const auto document = read_json(manifest_path);
    const auto& schema = require_member(document, "schema_version", "target manifest");
    if (!schema.is_number() || schema.as_number() != 1.0)
        throw std::runtime_error("target manifest schema_version must be 1");
    BuildModel result;
    result.source_root = normalized_path(require_string(document, "source_root", "target manifest"),
                                         manifest_path.parent_path());
    result.build_root = normalized_path(require_string(document, "build_root", "target manifest"),
                                        manifest_path.parent_path());
    for (const auto& configuration_value :
         require_array(document, "configurations", "target manifest")) {
        BuildConfiguration configuration;
        configuration.name = require_string(configuration_value, "name", "manifest configuration");
        for (const auto& target_value :
             require_array(configuration_value, "targets", "manifest configuration")) {
            BuildTarget target;
            target.id = require_string(target_value, "id", "manifest target");
            target.name = require_string(target_value, "name", "manifest target");
            target.kind =
                parse_target_kind(require_string(target_value, "type", "manifest target"));
            for (const auto& source : require_array(target_value, "sources", "manifest target")) {
                if (!source.is_string())
                    throw std::runtime_error("manifest target source must be a string");
                target.sources.push_back(normalized_path(source.as_string(), result.source_root));
            }
            if (const auto* dependencies = target_value.find("dependencies");
                dependencies != nullptr) {
                if (!dependencies->is_array())
                    throw std::runtime_error("manifest target dependencies must be an array");
                for (const auto& dependency : dependencies->as_array()) {
                    if (!dependency.is_string() || dependency.as_string().empty())
                        throw std::runtime_error("manifest target dependency must be a string");
                    target.dependencies.push_back(dependency.as_string());
                }
            }
            if (const auto* artifacts = target_value.find("artifacts"); artifacts != nullptr) {
                if (!artifacts->is_array())
                    throw std::runtime_error("manifest target artifacts must be an array");
                for (const auto& artifact : artifacts->as_array()) {
                    if (!artifact.is_string())
                        throw std::runtime_error("manifest target artifact must be a string");
                    target.artifacts.push_back(
                        normalized_path(artifact.as_string(), result.build_root));
                }
            }
            configuration.targets.push_back(std::move(target));
        }
        result.configurations.push_back(std::move(configuration));
    }
    if (result.configurations.empty())
        throw std::runtime_error("target manifest has no configurations");
    return result;
}

TargetSelection select_target_commands(const BuildModel& model, std::string_view configuration_name,
                                       std::string_view target_name,
                                       const std::vector<CompileCommand>& commands) {
    const auto& configuration = select_configuration(model, configuration_name);
    const auto& selected_target = select_target(configuration, target_name);
    if (selected_target.kind == BuildTargetKind::Utility)
        throw std::runtime_error("utility targets cannot be analyzed: " + selected_target.name);

    std::unordered_map<std::string, const BuildTarget*> by_id;
    for (const auto& target : configuration.targets) {
        if (!by_id.emplace(target.id, &target).second)
            throw std::runtime_error("duplicate target id in build configuration: " + target.id);
    }

    TargetSelection result;
    result.context.configuration = configuration.name;
    result.context.target_id = selected_target.id;
    result.context.target_name = selected_target.name;
    result.context.target_kind = selected_target.kind;

    std::vector<const BuildTarget*> closure;
    std::set<std::string, std::less<>> visited;
    const auto visit = [&](const auto& self, const BuildTarget& target) -> void {
        if (!visited.insert(target.id).second)
            return;
        closure.push_back(&target);
        for (const auto& dependency_id : target.dependencies) {
            const auto dependency = by_id.find(dependency_id);
            if (dependency == by_id.end()) {
                result.diagnostics.push_back("build metadata omits dependency target " +
                                             dependency_id + " referenced by " + target.name);
                continue;
            }
            self(self, *dependency->second);
        }
    };
    visit(visit, selected_target);
    std::ranges::sort(closure, {}, &BuildTarget::name);
    for (const auto* target : closure) {
        result.context.closure_targets.push_back(target->name);
        if (target->kind == BuildTargetKind::StaticLibrary) {
            result.diagnostics.push_back(
                "static-library target " + target->name +
                " is analyzed with all translation units present; archive-member extraction is "
                "not modeled");
        }
    }

    std::set<std::size_t> selected_indexes;
    for (const auto* target : closure) {
        if (target->kind == BuildTargetKind::Utility ||
            target->kind == BuildTargetKind::InterfaceLibrary)
            continue;
        for (const auto& source : target->sources) {
            std::vector<std::size_t> candidates;
            for (std::size_t index = 0; index < commands.size(); ++index) {
                if (commands[index].file == source)
                    candidates.push_back(index);
            }
            if (candidates.empty())
                throw std::runtime_error("no compilation command for target source " +
                                         source.string() + " in " + target->name);
            std::vector<std::size_t> owned;
            for (const auto index : candidates) {
                if (command_matches_target(commands[index], *target))
                    owned.push_back(index);
            }
            std::vector<std::size_t> configured;
            for (const auto index : owned) {
                if (command_matches_configuration(commands[index], *target, configuration.name))
                    configured.push_back(index);
            }
            auto& usable = !configured.empty() ? configured : (owned.empty() ? candidates : owned);
            if (usable.size() > 1U) {
                const auto* first = &commands[usable.front()];
                const bool equivalent = std::ranges::all_of(usable, [&](std::size_t index) {
                    return equivalent_commands(first, &commands[index]);
                });
                if (!equivalent) {
                    throw std::runtime_error("ambiguous compilation commands for target source " +
                                             source.string() + " in " + target->name);
                }
            }
            selected_indexes.insert(usable.front());
        }
    }
    for (const auto index : selected_indexes)
        result.commands.push_back(commands[index]);
    if (result.commands.empty())
        throw std::runtime_error("selected target closure contains no C++ compilation commands");
    std::ranges::sort(result.diagnostics);
    return result;
}

std::string_view to_string(BuildTargetKind kind) {
    switch (kind) {
    case BuildTargetKind::Executable:
        return "executable";
    case BuildTargetKind::StaticLibrary:
        return "static_library";
    case BuildTargetKind::SharedLibrary:
        return "shared_library";
    case BuildTargetKind::ModuleLibrary:
        return "module_library";
    case BuildTargetKind::ObjectLibrary:
        return "object_library";
    case BuildTargetKind::InterfaceLibrary:
        return "interface_library";
    case BuildTargetKind::Utility:
        return "utility";
    }
    return "unknown";
}

} // namespace cxx_dead
