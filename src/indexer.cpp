#include "cxx_dead/indexer.h"

#include "cxx_dead/json.h"
#include "cxx_dead/process.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace cxx_dead {

namespace {

using json::Value;

struct RecordInfo {
    std::string name;
    std::vector<std::string> bases;
};

struct TranslationUnitState {
    const CompileCommand& command;
    const IndexOptions& options;
    Graph& graph;
    std::unordered_map<std::string, std::string> contexts;
    std::unordered_map<std::string, SymbolId> declarations;
    std::unordered_map<std::string, Symbol> opaque_declarations;
    std::unordered_map<std::string, std::vector<SymbolId>> callable_bindings;
    std::unordered_map<std::string, RecordInfo>& records;
    std::unordered_map<std::string, std::vector<std::size_t>> line_offsets;
    std::vector<std::string>& diagnostics;
    std::vector<bool>& registration_rule_matches;
};

bool is_function_kind(std::string_view kind) {
    return kind == "FunctionDecl" || kind == "CXXMethodDecl" || kind == "CXXConstructorDecl" ||
           kind == "CXXDestructorDecl" || kind == "CXXConversionDecl";
}

SymbolKind symbol_kind(std::string_view kind) {
    if (kind == "CXXConstructorDecl")
        return SymbolKind::Constructor;
    if (kind == "CXXDestructorDecl")
        return SymbolKind::Destructor;
    if (kind == "CXXMethodDecl" || kind == "CXXConversionDecl")
        return SymbolKind::Method;
    return SymbolKind::Function;
}

bool is_record_kind(std::string_view kind) {
    return kind == "CXXRecordDecl" || kind == "RecordDecl" ||
           kind == "ClassTemplateSpecializationDecl";
}

const Value::Array& children(const Value& node) {
    static const Value::Array empty;
    const auto* inner = node.find("inner");
    return inner != nullptr && inner->is_array() ? inner->as_array() : empty;
}

std::string join_name(std::string_view scope, std::string_view name) {
    if (name.empty())
        return std::string(scope);
    if (scope.empty())
        return std::string(name);
    return std::string(scope) + "::" + std::string(name);
}

std::string anonymous_scope_name(const Value& node) {
    const auto name = node.string_or("name");
    return name.empty() ? "(anonymous namespace)" : name;
}

std::filesystem::path absolute_normalized(std::filesystem::path path,
                                          const std::filesystem::path& base) {
    if (path.empty())
        return {};
    if (path.is_relative())
        path = base / path;
    return std::filesystem::absolute(path).lexically_normal();
}

const Value* physical_location(const Value* location) {
    if (location == nullptr || !location->is_object())
        return nullptr;
    if (location->find("file") != nullptr)
        return location;
    if (const auto* expansion = location->find("expansionLoc"); expansion != nullptr) {
        if (const auto* nested = physical_location(expansion); nested != nullptr)
            return nested;
    }
    if (const auto* spelling = location->find("spellingLoc"); spelling != nullptr) {
        if (const auto* nested = physical_location(spelling); nested != nullptr)
            return nested;
    }
    return location;
}

std::filesystem::path node_file(const Value& node, const std::filesystem::path& inherited,
                                const std::filesystem::path& directory,
                                const std::filesystem::path& main_source) {
    const auto* location = physical_location(node.find("loc"));
    if (location != nullptr) {
        const auto file = location->string_or("file");
        if (!file.empty() && file.front() != '<')
            return absolute_normalized(file, directory);
        // At translation-unit scope Clang may omit the physical header name while retaining an
        // includedFrom chain. Inheriting the main file here misattributes libstdc++ compatibility
        // helpers (for example __gthread_*) to the project source.
        if (location->find("includedFrom") != nullptr && inherited == main_source)
            return {};
    }
    return inherited;
}

std::size_t numeric_field(const Value* object, std::string_view key) {
    if (object == nullptr)
        return 0;
    const auto* value = object->find(key);
    if (value == nullptr || !value->is_number())
        return 0;
    return static_cast<std::size_t>(value->as_number());
}

const Value* location_variant(const Value* location, std::string_view variant) {
    if (location == nullptr || !location->is_object())
        return nullptr;
    if (const auto* nested = location->find(variant); nested != nullptr && nested->is_object())
        return nested;
    if (location->find("spellingLoc") == nullptr && location->find("expansionLoc") == nullptr &&
        variant == "spellingLoc") {
        return location;
    }
    return nullptr;
}

std::vector<std::size_t>& source_line_offsets(TranslationUnitState& state,
                                              const std::filesystem::path& file) {
    auto [entry, inserted] = state.line_offsets.try_emplace(file.string());
    if (!inserted)
        return entry->second;
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return entry->second;
    const std::string contents(std::istreambuf_iterator<char>(input), {});
    entry->second.push_back(0);
    for (std::size_t offset = 0; offset < contents.size(); ++offset) {
        if (contents[offset] == '\n')
            entry->second.push_back(offset + 1U);
    }
    return entry->second;
}

SourcePoint source_point(const Value* location, const std::filesystem::path& inherited_file,
                         TranslationUnitState& state) {
    SourcePoint point;
    if (location == nullptr)
        return point;
    auto file = location->string_or("file");
    if (!file.empty() && file.front() != '<')
        point.file = absolute_normalized(file, state.command.directory);
    else if (file.empty())
        point.file = inherited_file;
    point.line = numeric_field(location, "line");
    point.column = numeric_field(location, "col");
    point.offset = numeric_field(location, "offset");
    point.token_length = numeric_field(location, "tokLen");

    const auto* offset = location->find("offset");
    const bool has_offset = offset != nullptr && offset->is_number();
    if (!point.file.empty() && has_offset && (point.line == 0 || point.column == 0)) {
        const auto& offsets = source_line_offsets(state, point.file);
        if (!offsets.empty()) {
            const auto upper = std::ranges::upper_bound(offsets, point.offset);
            const auto line_index = static_cast<std::size_t>(std::distance(offsets.begin(), upper));
            if (point.line == 0)
                point.line = line_index;
            if (point.column == 0 && line_index != 0)
                point.column = point.offset - offsets[line_index - 1U] + 1U;
        }
    }
    return point;
}

SourceExtent source_extent(const Value& node, std::string_view variant,
                           const std::filesystem::path& inherited_file,
                           TranslationUnitState& state) {
    const auto* location = location_variant(node.find("loc"), variant);
    const auto* range = node.find("range");
    const auto* begin =
        range != nullptr ? location_variant(range->find("begin"), variant) : nullptr;
    const auto* end = range != nullptr ? location_variant(range->find("end"), variant) : nullptr;

    SourceExtent extent;
    extent.location = source_point(location, inherited_file, state);
    const auto range_file = !extent.location.file.empty() ? extent.location.file : inherited_file;
    extent.begin = source_point(begin, range_file, state);
    const auto end_file = !extent.begin.file.empty() ? extent.begin.file : range_file;
    extent.end = source_point(end, end_file, state);
    if (extent.location.file.empty())
        extent.location = extent.begin;
    if (extent.begin.file.empty())
        extent.begin = extent.location;
    if (extent.end.file.empty())
        extent.end = extent.begin;
    return extent;
}

bool has_expansion_location(const Value& node) {
    if (const auto* location = node.find("loc");
        location != nullptr && location->find("expansionLoc") != nullptr) {
        return true;
    }
    if (const auto* range = node.find("range"); range != nullptr) {
        return (range->find("begin") != nullptr &&
                range->find("begin")->find("expansionLoc") != nullptr) ||
               (range->find("end") != nullptr &&
                range->find("end")->find("expansionLoc") != nullptr);
    }
    return false;
}

SymbolSource symbol_source(const Value& node, const std::filesystem::path& inherited_file,
                           TranslationUnitState& state) {
    SymbolSource source{
        .spelling = source_extent(node, "spellingLoc", inherited_file, state),
        .expansion = std::nullopt,
    };
    if (has_expansion_location(node))
        source.expansion = source_extent(node, "expansionLoc", inherited_file, state);
    return source;
}

bool path_is_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    if (child.empty() || parent.empty())
        return false;
    const auto relative = child.lexically_relative(parent);
    if (relative.empty())
        return child == parent;
    const auto first = *relative.begin();
    return first != ".." && !relative.is_absolute();
}

std::string identity_path(const std::filesystem::path& path,
                          const std::filesystem::path& project_root) {
    if (path.empty())
        return {};
    if (path_is_within(path, project_root))
        return path.lexically_relative(project_root).generic_string();
    return path.lexically_normal().generic_string();
}

std::string fallback_identity_anchor(std::string_view qualified, std::string_view signature,
                                     const SymbolSource& source,
                                     const std::filesystem::path& project_root) {
    const auto& location =
        (source.expansion.has_value() ? *source.expansion : source.spelling).location;
    return std::string(qualified) + "|" + std::string(signature) + "|" +
           identity_path(location.file, project_root) + ":" + std::to_string(location.offset);
}

SymbolScope symbol_scope(const TranslationUnitState& state, const std::filesystem::path& file) {
    if (std::ranges::any_of(state.options.excluded_paths,
                            [&](const std::filesystem::path& excluded) {
                                return path_is_within(file, excluded);
                            })) {
        return SymbolScope::Excluded;
    }
    if (!path_is_within(file, state.options.project_root))
        return SymbolScope::ExternalOpaque;
    if (std::ranges::any_of(state.options.report_paths,
                            [&](const std::filesystem::path& report_path) {
                                return path_is_within(file, report_path);
                            })) {
        return SymbolScope::Reportable;
    }
    return SymbolScope::Indexed;
}

std::string type_string(const Value& node, std::string_view field = "type") {
    const auto* type = node.find(field);
    return type != nullptr ? type->string_or("qualType") : std::string{};
}

std::string desugared_type_string(const Value& node, std::string_view field = "type") {
    const auto* type = node.find(field);
    if (type == nullptr)
        return {};
    return type->string_or("desugaredQualType", type->string_or("qualType"));
}

bool has_body(const Value& node) {
    if (!node.string_or("explicitlyDefaulted").empty())
        return true;
    return std::ranges::any_of(children(node), [](const Value& child) {
        const auto kind = child.string_or("kind");
        return kind == "CompoundStmt" || kind == "CXXTryStmt" || kind == "CoroutineBodyStmt";
    });
}

bool has_override_attribute(const Value& node) {
    return std::ranges::any_of(children(node), [](const Value& child) {
        return child.string_or("kind") == "OverrideAttr";
    });
}

bool is_internal_symbol(const Value& node, std::string_view kind, std::string_view mangled,
                        std::string_view qualified) {
    const bool namespace_static =
        kind == "FunctionDecl" && node.string_or("storageClass") == "static";
    return namespace_static || mangled.starts_with("_ZL") ||
           mangled.find("GLOBAL__N") != std::string_view::npos ||
           qualified.find("(anonymous namespace)") != std::string_view::npos;
}

std::string normalize_type(std::string value) {
    const auto erase_all = [&](std::string_view needle) {
        for (auto position = value.find(needle); position != std::string::npos;
             position = value.find(needle)) {
            value.erase(position, needle.size());
        }
    };
    erase_all("const ");
    erase_all("volatile ");
    erase_all("struct ");
    erase_all("class ");
    while (!value.empty() && (value.back() == '&' || value.back() == '*' ||
                              std::isspace(static_cast<unsigned char>(value.back())) != 0)) {
        value.pop_back();
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    return value;
}

std::optional<std::string> owning_pointer_element_type(std::string_view type) {
    const auto normalized = normalize_type(std::string(type));
    for (const auto pointer_name :
         {std::string_view{"unique_ptr<"}, std::string_view{"shared_ptr<"}}) {
        const auto name = normalized.find(pointer_name);
        if (name == std::string_view::npos)
            continue;
        const auto prefix = std::string_view(normalized).substr(0, name);
        if (!prefix.starts_with("std::") || prefix.find_first_of("<>, ") != std::string_view::npos)
            continue;
        const auto begin = name + pointer_name.size();
        std::size_t depth = 0;
        for (std::size_t index = begin; index < normalized.size(); ++index) {
            if (normalized[index] == '<') {
                ++depth;
            } else if (normalized[index] == '>') {
                if (depth == 0) {
                    auto element = normalize_type(normalized.substr(begin, index - begin));
                    if (!element.empty() && !element.ends_with("[]"))
                        return element;
                    return std::nullopt;
                }
                --depth;
            } else if (normalized[index] == ',' && depth == 0) {
                auto element = normalize_type(normalized.substr(begin, index - begin));
                if (!element.empty() && !element.ends_with("[]"))
                    return element;
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

bool names_equivalent(std::string_view qualified, std::string_view possibly_relative) {
    if (qualified == possibly_relative)
        return true;
    return qualified.size() > possibly_relative.size() && qualified.ends_with(possibly_relative) &&
           qualified[qualified.size() - possibly_relative.size() - 1U] == ':';
}

void collect_contexts(const Value& node, TranslationUnitState& state, std::string scope,
                      std::filesystem::path inherited_file) {
    const auto file = node_file(node, inherited_file, state.command.directory, state.command.file);
    const auto kind = node.string_or("kind");
    auto child_scope = scope;

    if (kind == "NamespaceDecl") {
        child_scope = join_name(scope, anonymous_scope_name(node));
        if (const auto id = node.string_or("id"); !id.empty())
            state.contexts[id] = child_scope;
    } else if (is_record_kind(kind) && !node.bool_or("isImplicit")) {
        const auto name = node.string_or("name");
        if (!name.empty()) {
            child_scope = join_name(scope, name);
            if (has_indexed_body(symbol_scope(state, file))) {
                if (const auto id = node.string_or("id"); !id.empty())
                    state.contexts[id] = child_scope;
                auto& record = state.records[child_scope];
                record.name = child_scope;
                if (const auto* bases = node.find("bases"); bases != nullptr && bases->is_array()) {
                    for (const auto& base : bases->as_array()) {
                        const auto base_name = normalize_type(type_string(base));
                        if (!base_name.empty() &&
                            std::ranges::find(record.bases, base_name) == record.bases.end()) {
                            record.bases.push_back(base_name);
                        }
                    }
                }
            }
        }
    }

    for (const auto& child : children(node)) {
        collect_contexts(child, state, child_scope, file);
    }
}

void collect_declarations(const Value& node, TranslationUnitState& state, std::string scope,
                          std::filesystem::path inherited_file, bool tu_local_context = false) {
    const auto file = node_file(node, inherited_file, state.command.directory, state.command.file);
    const auto kind = node.string_or("kind");
    auto child_scope = scope;
    auto child_tu_local_context = tu_local_context;
    if (kind == "NamespaceDecl") {
        child_scope = join_name(scope, anonymous_scope_name(node));
    } else if (is_record_kind(kind) && !node.bool_or("isImplicit")) {
        const auto name = node.string_or("name");
        if (!name.empty())
            child_scope = join_name(scope, name);
    } else if (kind == "CXXRecordDecl") {
        const auto* definition = node.find("definitionData");
        if (definition != nullptr && definition->is_object() && definition->bool_or("isLambda"))
            child_tu_local_context = true;
    }
    if (is_function_kind(kind))
        child_tu_local_context = true;

    if (is_function_kind(kind) && !node.bool_or("isImplicit")) {
        const auto ast_id = node.string_or("id");
        const auto mangled = node.string_or("mangledName");
        const auto name = node.string_or("name");
        auto owner_scope = scope;
        if (const auto context_id = node.string_or("parentDeclContextId"); !context_id.empty()) {
            if (const auto context = state.contexts.find(context_id);
                context != state.contexts.end()) {
                owner_scope = context->second;
            }
        }
        const auto qualified = join_name(owner_scope, name);
        const bool internal = is_internal_symbol(node, kind, mangled, qualified);
        const auto signature = type_string(node);
        const auto source = symbol_source(node, file, state);
        const bool translation_unit_local = internal || tu_local_context;
        auto identity = make_symbol_identity(
            state.options.configuration_id, "", mangled,
            translation_unit_local ? identity_path(state.command.file, state.options.project_root)
                                   : std::string{},
            fallback_identity_anchor(qualified, signature, source, state.options.project_root));
        const auto key = stable_symbol_key(identity);
        const auto scope_kind = symbol_scope(state, file);
        if (!ast_id.empty() && !key.empty() && scope_kind != SymbolScope::Excluded) {
            Symbol symbol{
                .key = key,
                .identity = std::move(identity),
                .name = name,
                .qualified_name = qualified,
                .class_name = symbol_kind(kind) == SymbolKind::Function ? "" : owner_scope,
                .signature = signature,
                .source = source,
                .kind = symbol_kind(kind),
                .scope = scope_kind,
                // A FunctionDecl nested in a FunctionTemplateDecl describes the pattern and has no
                // mangled identity. Concrete specializations are indexed separately. Reporting the
                // pattern itself produces a false dead finding even when its instantiations are
                // live.
                .defined = has_indexed_body(scope_kind) && has_body(node) && !mangled.empty(),
                .internal_linkage = internal,
                .is_virtual = node.bool_or("virtual") || has_override_attribute(node),
            };
            if (scope_kind == SymbolScope::ExternalOpaque) {
                state.opaque_declarations.insert_or_assign(ast_id, std::move(symbol));
            } else {
                const auto symbol_id = state.graph.add_or_merge_symbol(std::move(symbol));
                state.declarations[ast_id] = symbol_id;
                if (name == "main" && has_body(node)) {
                    state.graph.add_root(
                        symbol_id, RootKind::ApplicationEntryPoint,
                        {.provider = "application_policy",
                         .reason = "defined function named main is an application entry point"});
                }
            }
        }
    }

    for (const auto& child : children(node)) {
        collect_declarations(child, state, child_scope, file, child_tu_local_context);
    }
}

void collect_reference_ids(const Value& node, std::vector<std::string>& output) {
    if (const auto* referenced = node.find("referencedDecl");
        referenced != nullptr && referenced->is_object()) {
        const auto kind = referenced->string_or("kind");
        if (is_function_kind(kind)) {
            if (const auto id = referenced->string_or("id"); !id.empty())
                output.push_back(id);
        }
    }
    if (const auto* member = node.find("referencedMemberDecl");
        member != nullptr && member->is_string()) {
        output.push_back(member->as_string());
    }
    for (const auto& child : children(node))
        collect_reference_ids(child, output);
}

std::vector<SymbolId> resolve_references(const Value& expression, TranslationUnitState& state) {
    std::vector<std::string> ast_ids;
    collect_reference_ids(expression, ast_ids);
    std::vector<SymbolId> result;
    for (const auto& ast_id : ast_ids) {
        auto found = state.declarations.find(ast_id);
        if (found == state.declarations.end()) {
            if (const auto opaque = state.opaque_declarations.find(ast_id);
                opaque != state.opaque_declarations.end()) {
                const auto symbol = state.graph.add_or_merge_symbol(opaque->second);
                found = state.declarations.emplace(ast_id, symbol).first;
            }
        }
        if (found != state.declarations.end() &&
            std::ranges::find(result, found->second) == result.end()) {
            result.push_back(found->second);
        }
    }
    return result;
}

void append_unique(std::vector<SymbolId>& destination, const std::vector<SymbolId>& source) {
    for (const auto id : source) {
        if (std::ranges::find(destination, id) == destination.end())
            destination.push_back(id);
    }
}

bool is_call_expression(std::string_view kind);

void collect_lambda_operators(const Value& node, TranslationUnitState& state,
                              std::vector<SymbolId>& output) {
    if (node.string_or("kind") == "CXXMethodDecl" && node.string_or("name") == "operator()") {
        if (const auto found = state.declarations.find(node.string_or("id"));
            found != state.declarations.end() &&
            std::ranges::find(output, found->second) == output.end()) {
            output.push_back(found->second);
        }
        return;
    }
    for (const auto& child : children(node))
        collect_lambda_operators(child, state, output);
}

std::vector<SymbolId> resolve_callable_targets(const Value& expression,
                                               TranslationUnitState& state) {
    const auto kind = expression.string_or("kind");
    std::vector<SymbolId> result;
    if (kind == "LambdaExpr") {
        collect_lambda_operators(expression, state, result);
        return result;
    }
    if (kind == "DeclRefExpr") {
        if (const auto* referenced = expression.find("referencedDecl");
            referenced != nullptr && referenced->is_object() &&
            referenced->string_or("kind") == "VarDecl") {
            if (const auto binding = state.callable_bindings.find(referenced->string_or("id"));
                binding != state.callable_bindings.end()) {
                append_unique(result, binding->second);
            }
        }
    }
    if (kind == "DeclRefExpr" || kind == "MemberExpr")
        append_unique(result, resolve_references(expression, state));

    const auto callable_type = normalize_type(desugared_type_string(expression));
    if (!callable_type.empty()) {
        for (SymbolId id = 0; id < state.graph.symbols().size(); ++id) {
            const auto& symbol = state.graph.symbols()[id];
            if (symbol.name == "operator()" && names_equivalent(symbol.class_name, callable_type) &&
                std::ranges::find(result, id) == result.end()) {
                result.push_back(id);
            }
        }
    }

    if (!is_call_expression(kind) && !is_function_kind(kind)) {
        for (const auto& child : children(expression))
            append_unique(result, resolve_callable_targets(child, state));
    }
    return result;
}

bool rule_matches(const Symbol& symbol, const CallbackRegistrationRule& rule) {
    return symbol.qualified_name == rule.callee || symbol.identity.linkage_name == rule.callee ||
           symbol.key == rule.callee;
}

std::string referenced_variable_id(const Value& expression) {
    if (expression.string_or("kind") == "DeclRefExpr") {
        if (const auto* referenced = expression.find("referencedDecl");
            referenced != nullptr && referenced->is_object() &&
            referenced->string_or("kind") == "VarDecl") {
            return referenced->string_or("id");
        }
    }
    for (const auto& child : children(expression)) {
        if (auto id = referenced_variable_id(child); !id.empty())
            return id;
    }
    return {};
}

bool is_call_expression(std::string_view kind) {
    return kind == "CallExpr" || kind == "CXXMemberCallExpr" || kind == "CXXOperatorCallExpr" ||
           kind == "CUDAKernelCallExpr";
}

void add_use(TranslationUnitState& state, std::optional<SymbolId> caller, SymbolId target,
             EdgeKind kind, bool global_initializer, std::string reason = {}) {
    if (caller.has_value()) {
        if (reason.empty()) {
            switch (kind) {
            case EdgeKind::DirectCall:
                reason = "direct call expression";
                break;
            case EdgeKind::Constructs:
                reason = "constructor or destructor use";
                break;
            case EdgeKind::VirtualDispatch:
                reason = "virtual dispatch";
                break;
            case EdgeKind::CallbackRegistration:
                reason = "configured callback registration";
                break;
            }
        }
        state.graph.add_edge(*caller, target, kind,
                             {.provider = "clang_ast", .reason = std::move(reason)});
    } else if (global_initializer) {
        state.graph.add_root(
            target, RootKind::GlobalInitializer,
            {.provider = "clang_ast", .reason = "use from a namespace-scope variable initializer"});
    }
}

bool add_construction_edges(TranslationUnitState& state, std::optional<SymbolId> caller,
                            std::string constructed_type,
                            std::optional<std::string_view> constructor_signature,
                            bool global_initializer, std::string_view reason) {
    constructed_type = normalize_type(std::move(constructed_type));
    if (constructed_type.empty())
        return false;
    bool matched = false;
    for (SymbolId id = 0; id < state.graph.symbols().size(); ++id) {
        const auto& symbol = state.graph.symbols()[id];
        if (!names_equivalent(symbol.class_name, constructed_type))
            continue;
        const bool selected_constructor =
            symbol.kind == SymbolKind::Constructor &&
            (!constructor_signature.has_value() || symbol.signature == *constructor_signature);
        if (!selected_constructor && symbol.kind != SymbolKind::Destructor)
            continue;
        add_use(state, caller, id, EdgeKind::Constructs, global_initializer, std::string(reason));
        matched = true;
    }
    return matched;
}

std::string referenced_function_name(const Value& node) {
    if (const auto* referenced = node.find("referencedDecl");
        referenced != nullptr && referenced->is_object() &&
        is_function_kind(referenced->string_or("kind"))) {
        return referenced->string_or("name");
    }
    for (const auto& child : children(node)) {
        if (auto name = referenced_function_name(child); !name.empty())
            return name;
    }
    return {};
}

void add_factory_construction_edges(TranslationUnitState& state, std::optional<SymbolId> caller,
                                    const Value& call, const Value& callee,
                                    bool global_initializer) {
    if (call.string_or("valueCategory") != "prvalue")
        return;
    const auto result_type = desugared_type_string(call);
    const auto element_type = owning_pointer_element_type(result_type);
    if (!element_type.has_value())
        return;
    const auto helper = referenced_function_name(callee);
    if (helper.empty())
        return;
    const bool standard_factory = helper == "make_unique" || helper == "make_shared";
    const auto reason = standard_factory ? "standard smart-pointer factory construction"
                                         : "conservative owning-pointer factory construction";
    if (!add_construction_edges(state, caller, *element_type, std::nullopt, global_initializer,
                                reason)) {
        return;
    }
    if (!standard_factory) {
        state.diagnostics.push_back("unsupported owning-pointer factory " + helper +
                                    "; conservatively retained construction and destruction for " +
                                    *element_type);
    }
}

void collect_uses(const Value& node, TranslationUnitState& state, std::optional<SymbolId> caller,
                  bool global_initializer, bool callee_position, bool top_level) {
    const auto kind = node.string_or("kind");
    if (is_function_kind(kind)) {
        if (const auto declaration = state.declarations.find(node.string_or("id"));
            declaration != state.declarations.end()) {
            if (!has_indexed_body(state.graph.symbols()[declaration->second].scope))
                return;
            caller = declaration->second;
        } else {
            // System-header function bodies do not contribute project-to-project edges. Avoid
            // traversing their often enormous template-instantiation subtrees.
            return;
        }
        global_initializer = false;
    }

    if (kind == "VarDecl" && top_level && !caller.has_value())
        global_initializer = true;

    if (kind == "VarDecl") {
        std::vector<SymbolId> targets;
        for (const auto& child : children(node))
            append_unique(targets, resolve_callable_targets(child, state));
        if (!targets.empty())
            state.callable_bindings.insert_or_assign(node.string_or("id"), std::move(targets));
    }

    if ((kind == "BinaryOperator" && node.string_or("opcode") == "=") ||
        kind == "CompoundAssignOperator") {
        if (!children(node).empty())
            state.callable_bindings.erase(referenced_variable_id(children(node).front()));
    }

    if (is_call_expression(kind)) {
        const auto& inner = children(node);
        if (!inner.empty()) {
            auto targets = resolve_references(inner.front(), state);
            if (targets.empty())
                targets = resolve_callable_targets(inner.front(), state);
            for (const auto target : targets) {
                add_use(state, caller, target, EdgeKind::DirectCall, global_initializer);
            }
            add_factory_construction_edges(state, caller, node, inner.front(), global_initializer);
            for (std::size_t argument = 1; argument < inner.size(); ++argument) {
                const auto callable_targets = resolve_callable_targets(inner[argument], state);
                for (const auto target : callable_targets) {
                    state.graph.add_escape(
                        target, EscapeKind::CallableObject,
                        {.provider = "clang_ast",
                         .reason = "callable value passed outside a direct callee position"},
                        caller);
                }
            }
            for (std::size_t rule_index = 0;
                 rule_index < state.options.callback_registration_rules.size(); ++rule_index) {
                const auto& rule = state.options.callback_registration_rules[rule_index];
                const bool matched_callee = std::ranges::any_of(targets, [&](SymbolId target) {
                    return rule_matches(state.graph.symbols()[target], rule);
                });
                if (!matched_callee)
                    continue;
                state.registration_rule_matches[rule_index] = true;
                if (rule.argument_index + 1U >= inner.size()) {
                    throw std::runtime_error("callback registration rule " + rule.callee + ":" +
                                             std::to_string(rule.argument_index) +
                                             " exceeds the registrar argument list");
                }
                const auto callbacks =
                    resolve_callable_targets(inner[rule.argument_index + 1U], state);
                if (callbacks.empty()) {
                    throw std::runtime_error("callback registration rule " + rule.callee + ":" +
                                             std::to_string(rule.argument_index) +
                                             " did not resolve a callable argument");
                }
                const auto reason = "configured callback argument " +
                                    std::to_string(rule.argument_index) + " of " + rule.callee;
                for (const auto callback : callbacks) {
                    if (caller.has_value()) {
                        state.graph.add_edge(
                            *caller, callback, EdgeKind::CallbackRegistration,
                            {.provider = "callback_registration", .reason = reason});
                    } else if (global_initializer) {
                        state.graph.add_root(
                            callback, RootKind::CallbackRegistration,
                            {.provider = "callback_registration",
                             .reason = reason + " from a namespace-scope initializer"});
                    }
                }
            }
            collect_uses(inner.front(), state, caller, global_initializer, true, false);
            for (std::size_t index = 1; index < inner.size(); ++index) {
                collect_uses(inner[index], state, caller, global_initializer, false, false);
            }
        }
        return;
    }

    if (kind == "CXXConstructExpr" || kind == "CXXTemporaryObjectExpr") {
        const auto constructor_type = type_string(node, "ctorType");
        const auto selected = constructor_type.empty()
                                  ? std::optional<std::string_view>{}
                                  : std::optional<std::string_view>{constructor_type};
        add_construction_edges(state, caller, desugared_type_string(node), selected,
                               global_initializer, "constructor or destructor use");
        if (normalize_type(desugared_type_string(node)).find("std::function<") !=
            std::string::npos) {
            for (const auto& child : children(node)) {
                for (const auto target : resolve_callable_targets(child, state)) {
                    state.graph.add_escape(target, EscapeKind::CallableObject,
                                           {.provider = "clang_ast",
                                            .reason = "callable value stored in std::function"},
                                           caller);
                }
            }
        }
    }

    if (!callee_position && (kind == "DeclRefExpr" || kind == "MemberExpr")) {
        for (const auto target : resolve_references(node, state)) {
            state.graph.add_escape(target, EscapeKind::AddressTaken,
                                   {.provider = "clang_ast",
                                    .reason = "function referenced outside a callee position"},
                                   caller);
        }
    }

    const bool child_top_level = kind == "TranslationUnitDecl";
    for (const auto& child : children(node)) {
        collect_uses(child, state, caller, global_initializer, callee_position, child_top_level);
    }
}

std::size_t compiler_argument_index(const std::vector<std::string>& arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto name = std::filesystem::path(arguments[index]).filename().string();
        if (name.find("clang") != std::string::npos || name == "c++" || name == "g++" ||
            name == "gcc" || name == "cc") {
            return index;
        }
    }
    return 0;
}

std::vector<std::string> ast_command(const CompileCommand& command, const IndexOptions& options) {
    std::vector<std::string> result{options.clang_executable};
    const auto compiler_index = compiler_argument_index(command.arguments);
    const std::set<std::string, std::less<>> flags_with_value{
        "-o", "-MF", "-MT", "-MQ", "-MJ", "--serialize-diagnostics", "-dependency-file"};
    const std::set<std::string, std::less<>> removed_flags{
        "-c", "-S", "-E", "-MD", "-MMD", "-MP", "-MG", "-MM", "-M", "-emit-llvm"};

    for (std::size_t index = compiler_index + 1U; index < command.arguments.size(); ++index) {
        const auto& argument = command.arguments[index];
        if (flags_with_value.contains(argument)) {
            ++index;
            continue;
        }
        if (removed_flags.contains(argument) || argument.starts_with("-o") ||
            argument.starts_with("-MF") || argument.starts_with("-MT") ||
            argument.starts_with("-MQ") || argument.starts_with("-MJ")) {
            continue;
        }
        if (!argument.empty() && argument.front() != '-') {
            const auto possible_file = absolute_normalized(argument, command.directory);
            if (possible_file == command.file)
                continue;
        }
        result.push_back(argument);
    }
    result.emplace_back("-Wno-error");
    result.emplace_back("-Xclang");
    result.emplace_back("-ast-dump=json");
    if (!options.ast_filter.empty()) {
        result.emplace_back("-Xclang");
        result.push_back("-ast-dump-filter=" + options.ast_filter);
    }
    result.emplace_back("-fsyntax-only");
    result.push_back(command.file.string());
    return result;
}

std::vector<std::string_view> split_json_documents(std::string_view input) {
    std::vector<std::string_view> documents;
    std::size_t start = std::string_view::npos;
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (std::size_t index = 0; index < input.size(); ++index) {
        const char character = input[index];
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                in_string = false;
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{' || character == '[') {
            if (depth == 0)
                start = index;
            ++depth;
        } else if (character == '}' || character == ']') {
            if (depth == 0)
                throw std::runtime_error("unexpected closing delimiter in Clang AST JSON");
            --depth;
            if (depth == 0) {
                documents.push_back(input.substr(start, index - start + 1U));
                start = std::string_view::npos;
            }
        } else if (depth == 0 && std::isspace(static_cast<unsigned char>(character)) == 0) {
            throw std::runtime_error("unexpected content between Clang AST JSON documents");
        }
    }
    if (depth != 0 || in_string)
        throw std::runtime_error("incomplete Clang AST JSON document");
    return documents;
}

std::string concise_diagnostic(std::string diagnostic) {
    constexpr std::size_t maximum = 4000;
    if (diagnostic.size() > maximum) {
        diagnostic.resize(maximum);
        diagnostic += "\n... diagnostics truncated";
    }
    return diagnostic;
}

[[noreturn]] void fail_indexing(const std::vector<CompileCommand>& commands,
                                const IndexResult& partial, std::size_t current,
                                TranslationUnitStatus status, std::string stage,
                                std::string message, std::optional<int> exit_code = std::nullopt,
                                std::optional<int> signal = std::nullopt,
                                RunState state = RunState::Incomplete) {
    RunDiagnostics diagnostics{
        .state = state,
        .frontend = partial.frontend,
        .partial_graph_discarded =
            !partial.translation_unit_diagnostics.empty() || !partial.graph.symbols().empty(),
        .translation_units = partial.translation_unit_diagnostics,
    };
    if (current < commands.size()) {
        diagnostics.translation_units.push_back({
            .file = commands[current].file,
            .status = status,
            .stage = std::move(stage),
            .message = message,
            .exit_code = exit_code,
            .signal = signal,
        });
        for (std::size_t index = current + 1U; index < commands.size(); ++index) {
            diagnostics.translation_units.push_back({
                .file = commands[index].file,
                .status = TranslationUnitStatus::Skipped,
                .stage = "indexing",
                .message = "not indexed because an earlier translation unit did not complete",
                .exit_code = std::nullopt,
                .signal = std::nullopt,
            });
        }
    }
    throw IndexingError(std::move(message), std::move(diagnostics));
}

std::optional<std::string>
resolve_record_name(std::string_view owner, std::string_view base,
                    const std::unordered_map<std::string, RecordInfo>& records) {
    if (records.contains(std::string(base)))
        return std::string(base);
    auto scope = std::string(owner);
    if (const auto position = scope.rfind("::"); position != std::string::npos) {
        scope.resize(position);
        const auto contextual = join_name(scope, base);
        if (records.contains(contextual))
            return contextual;
    }
    for (const auto& [name, record] : records) {
        static_cast<void>(record);
        if (names_equivalent(name, base))
            return name;
    }
    return std::nullopt;
}

void add_virtual_dispatch_edges(Graph& graph,
                                const std::unordered_map<std::string, RecordInfo>& records) {
    std::unordered_map<std::string, std::vector<SymbolId>> methods_by_class;
    for (SymbolId id = 0; id < graph.symbols().size(); ++id) {
        const auto& symbol = graph.symbols()[id];
        if (!symbol.class_name.empty() && symbol.kind == SymbolKind::Method) {
            methods_by_class[symbol.class_name].push_back(id);
        }
    }

    for (const auto& [derived_name, derived_methods] : methods_by_class) {
        const auto record = records.find(derived_name);
        if (record == records.end())
            continue;
        std::vector<std::string> pending = record->second.bases;
        std::unordered_set<std::string> visited;
        while (!pending.empty()) {
            auto base = std::move(pending.back());
            pending.pop_back();
            const auto resolved = resolve_record_name(derived_name, base, records);
            if (!resolved.has_value() || !visited.insert(*resolved).second)
                continue;
            if (const auto base_record = records.find(*resolved); base_record != records.end()) {
                pending.insert(pending.end(), base_record->second.bases.begin(),
                               base_record->second.bases.end());
            }
            const auto base_methods = methods_by_class.find(*resolved);
            if (base_methods == methods_by_class.end())
                continue;
            for (const auto derived : derived_methods) {
                const auto& derived_symbol = graph.symbols()[derived];
                for (const auto base_id : base_methods->second) {
                    const auto& base_symbol = graph.symbols()[base_id];
                    if (base_symbol.is_virtual && base_symbol.name == derived_symbol.name &&
                        base_symbol.signature == derived_symbol.signature) {
                        graph.add_edge(base_id, derived, EdgeKind::VirtualDispatch,
                                       {.provider = "class_hierarchy",
                                        .reason = "known override of a virtual method"});
                    }
                }
            }
        }
    }
}

void add_fallback_identity_diagnostics(IndexResult& result) {
    for (const auto& symbol : result.graph.symbols()) {
        if (symbol.identity.quality != IdentityQuality::Fallback)
            continue;
        result.diagnostics.push_back(
            "fallback symbol identity for " + symbol.qualified_name + " : " + symbol.signature +
            " at " + symbol.identity.fallback_anchor +
            "; this frontend exposes neither a linkage name nor a Clang USR");
    }
}

} // namespace

ClangAstIndexer::ClangAstIndexer(IndexOptions options) : options_(std::move(options)) {
    if (options_.configuration_id.empty())
        throw std::invalid_argument("configuration identity cannot be empty");
    options_.project_root = std::filesystem::absolute(options_.project_root).lexically_normal();
    if (options_.report_paths.empty())
        options_.report_paths.push_back(options_.project_root);
    for (auto& report_path : options_.report_paths) {
        report_path = std::filesystem::absolute(report_path).lexically_normal();
        if (!path_is_within(report_path, options_.project_root)) {
            throw std::invalid_argument("report path is outside the project root: " +
                                        report_path.string());
        }
    }
    for (auto& excluded : options_.excluded_paths)
        excluded = std::filesystem::absolute(excluded).lexically_normal();
}

IndexResult ClangAstIndexer::index(const std::vector<CompileCommand>& commands) const {
    if (commands.empty())
        throw std::runtime_error("compilation database contains no commands");
    IndexResult result;
    result.frontend = IndexFrontend::AstJson;
    std::unordered_map<std::string, RecordInfo> records;
    std::vector<bool> registration_rule_matches(options_.callback_registration_rules.size(), false);
    const auto index_started = std::chrono::steady_clock::now();

    for (std::size_t command_index = 0; command_index < commands.size(); ++command_index) {
        const auto& command = commands[command_index];
        const auto translation_unit_started = std::chrono::steady_clock::now();
        if (options_.cancellation_requested && options_.cancellation_requested()) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Cancelled,
                          "indexing", "indexing cancelled before " + command.file.string());
        }

        std::optional<std::chrono::milliseconds> process_timeout;
        std::string timeout_scope = "translation-unit";
        if (options_.translation_unit_timeout.count() > 0)
            process_timeout = options_.translation_unit_timeout;
        if (options_.index_timeout.count() > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - index_started);
            if (elapsed >= options_.index_timeout) {
                fail_indexing(commands, result, command_index, TranslationUnitStatus::TimedOut,
                              "indexing", "index timeout reached before " + command.file.string());
            }
            const auto remaining = options_.index_timeout - elapsed;
            if (!process_timeout.has_value() || remaining < *process_timeout) {
                process_timeout = remaining;
                timeout_scope = "index";
            }
        }

        const auto invocation = ast_command(command, options_);
        ProcessResult process;
        try {
            process = run_process(invocation, command.directory,
                                  {.timeout = process_timeout,
                                   .standard_output_limit = options_.max_ast_bytes,
                                   .cancellation_requested = options_.cancellation_requested});
        } catch (const std::exception& error) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Failed, "process",
                          "could not run Clang AST indexing for " + command.file.string() + ": " +
                              error.what());
        }
        if (process.termination == ProcessTermination::TimedOut) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::TimedOut, "clang",
                          timeout_scope + " timeout while indexing " + command.file.string(),
                          process.exit_code, process.signal);
        }
        if (process.termination == ProcessTermination::Cancelled) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Cancelled,
                          "clang", "indexing cancelled while processing " + command.file.string(),
                          process.exit_code, process.signal);
        }
        if (process.termination == ProcessTermination::OutputLimitExceeded) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Failed,
                          "ast_output",
                          "Clang AST output exceeded --max-ast-bytes for " + command.file.string(),
                          process.exit_code, process.signal);
        }
        if (process.exit_code != 0) {
            const auto message = "Clang AST indexing failed for " + command.file.string() +
                                 " (exit " + std::to_string(process.exit_code) + "):\n" +
                                 concise_diagnostic(process.standard_error);
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Failed, "clang",
                          message, process.exit_code, process.signal);
        }
        if (options_.verbose && !process.standard_error.empty()) {
            result.diagnostics.push_back(command.file.string() + ":\n" +
                                         concise_diagnostic(process.standard_error));
        }
        result.ast_bytes += process.standard_output.size();

        std::vector<Value> ast_documents;
        try {
            const auto documents = split_json_documents(process.standard_output);
            ast_documents.reserve(documents.size());
            for (const auto document : documents)
                ast_documents.push_back(json::parse(document));
        } catch (const std::exception& error) {
            fail_indexing(
                commands, result, command_index, TranslationUnitStatus::Failed, "ast_parse",
                "invalid Clang AST JSON for " + command.file.string() + ": " + error.what());
        }
        Graph translation_unit_facts;
        TranslationUnitState state{
            .command = command,
            .options = options_,
            .graph = translation_unit_facts,
            .contexts = {},
            .declarations = {},
            .opaque_declarations = {},
            .callable_bindings = {},
            .records = records,
            .line_offsets = {},
            .diagnostics = result.diagnostics,
            .registration_rule_matches = registration_rule_matches,
        };
        try {
            for (const auto& ast : ast_documents)
                collect_contexts(ast, state, "", command.file);
            for (const auto& ast : ast_documents)
                collect_declarations(ast, state, "", command.file);
            for (const auto& ast : ast_documents)
                collect_uses(ast, state, std::nullopt, false, false, false);
        } catch (const std::exception& error) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Failed,
                          "fact_collection",
                          "could not collect Clang AST facts for " + command.file.string() + ": " +
                              error.what());
        }
        if (options_.cancellation_requested && options_.cancellation_requested()) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Cancelled,
                          "fact_collection",
                          "indexing cancelled while collecting facts for " + command.file.string());
        }
        if (options_.translation_unit_timeout.count() > 0 &&
            std::chrono::steady_clock::now() - translation_unit_started >=
                options_.translation_unit_timeout) {
            fail_indexing(
                commands, result, command_index, TranslationUnitStatus::TimedOut, "fact_collection",
                "translation-unit timeout while collecting facts for " + command.file.string());
        }
        if (options_.index_timeout.count() > 0 &&
            std::chrono::steady_clock::now() - index_started >= options_.index_timeout) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::TimedOut,
                          "fact_collection",
                          "index timeout while collecting facts for " + command.file.string());
        }
        try {
            result.fact_bytes += graph_fact_bytes(translation_unit_facts);
            merge_graph(result.graph, translation_unit_facts);
        } catch (const std::exception& error) {
            fail_indexing(commands, result, command_index, TranslationUnitStatus::Failed,
                          "fact_merge",
                          "could not merge Clang AST facts for " + command.file.string() + ": " +
                              error.what());
        }
        ++result.translation_units;
        result.translation_unit_diagnostics.push_back({
            .file = command.file,
            .status = TranslationUnitStatus::Indexed,
            .stage = "indexing",
            .message = "translation unit indexed successfully",
            .exit_code = std::nullopt,
            .signal = std::nullopt,
        });
    }

    add_virtual_dispatch_edges(result.graph, records);

    for (std::size_t index = 0; index < registration_rule_matches.size(); ++index) {
        if (!registration_rule_matches[index]) {
            const auto& rule = options_.callback_registration_rules[index];
            throw IndexingError("callback registration rule did not match a registrar call: " +
                                    rule.callee + ":" + std::to_string(rule.argument_index),
                                {.state = RunState::Incomplete,
                                 .frontend = result.frontend,
                                 .partial_graph_discarded = true,
                                 .translation_units = result.translation_unit_diagnostics});
        }
    }

    for (const auto& requested : options_.manual_roots) {
        bool matched = false;
        for (SymbolId id = 0; id < result.graph.symbols().size(); ++id) {
            const auto& symbol = result.graph.symbols()[id];
            if (symbol.qualified_name == requested || symbol.identity.linkage_name == requested ||
                symbol.key == requested) {
                result.graph.add_root(id, RootKind::Manual,
                                      {.provider = "command_line",
                                       .reason = "matched configured root: " + requested});
                matched = true;
            }
        }
        if (!matched)
            result.diagnostics.push_back("configured root did not match a symbol: " + requested);
    }
    if (!options_.ast_filter.empty())
        result.diagnostics.push_back("Clang AST name filter active: " + options_.ast_filter);
    if (result.graph.roots().empty()) {
        throw IndexingError(
            "no application roots found; include main() or provide a matching --root symbol",
            {.state = RunState::Incomplete,
             .frontend = result.frontend,
             .partial_graph_discarded = true,
             .translation_units = result.translation_unit_diagnostics});
    }
    add_fallback_identity_diagnostics(result);
    try {
        result.graph.canonicalize();
    } catch (const std::exception& error) {
        throw IndexingError("could not finalize Clang AST facts: " + std::string(error.what()),
                            {.state = RunState::Incomplete,
                             .frontend = result.frontend,
                             .partial_graph_discarded = true,
                             .translation_units = result.translation_unit_diagnostics});
    }
    std::ranges::sort(result.translation_unit_diagnostics, {}, &TranslationUnitDiagnostic::file);
    std::ranges::sort(result.diagnostics);
    const auto duplicate = std::ranges::unique(result.diagnostics);
    result.diagnostics.erase(duplicate.begin(), duplicate.end());
    return result;
}

std::string_view to_string(IndexFrontend frontend) {
    switch (frontend) {
    case IndexFrontend::AstJson:
        return "ast-json";
    case IndexFrontend::LibTooling:
        return "libtooling";
    }
    return "unknown";
}

std::string_view to_string(RunState state) {
    switch (state) {
    case RunState::Complete:
        return "complete";
    case RunState::Incomplete:
        return "incomplete";
    case RunState::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

std::string_view to_string(TranslationUnitStatus status) {
    switch (status) {
    case TranslationUnitStatus::Indexed:
        return "indexed";
    case TranslationUnitStatus::Failed:
        return "failed";
    case TranslationUnitStatus::Skipped:
        return "skipped";
    case TranslationUnitStatus::TimedOut:
        return "timed_out";
    case TranslationUnitStatus::Unsupported:
        return "unsupported";
    case TranslationUnitStatus::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

} // namespace cxx_dead
