#include "cxx_dead/indexer.h"

#include "cxx_dead/json.h"
#include "cxx_dead/process.h"

#include <algorithm>
#include <cctype>
#include <functional>
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
    std::unordered_map<std::string, RecordInfo>& records;
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

std::size_t node_line(const Value& node) {
    if (const auto* location = physical_location(node.find("loc")); location != nullptr) {
        if (const auto line = numeric_field(location, "line"); line != 0)
            return line;
    }
    if (const auto* range = node.find("range"); range != nullptr) {
        if (const auto* begin = physical_location(range->find("begin")); begin != nullptr) {
            return numeric_field(begin, "line");
        }
    }
    return 0;
}

std::size_t node_end_line(const Value& node, std::size_t fallback) {
    if (const auto* range = node.find("range"); range != nullptr) {
        if (const auto* end = physical_location(range->find("end")); end != nullptr) {
            if (const auto line = numeric_field(end, "line"); line != 0)
                return line;
        }
    }
    return fallback;
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

bool is_project_file(const TranslationUnitState& state, const std::filesystem::path& file) {
    return path_is_within(file, state.options.project_root) &&
           std::ranges::none_of(state.options.excluded_paths,
                                [&](const std::filesystem::path& excluded) {
                                    return path_is_within(file, excluded);
                                });
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
            if (is_project_file(state, file)) {
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
                          std::filesystem::path inherited_file) {
    const auto file = node_file(node, inherited_file, state.command.directory, state.command.file);
    const auto kind = node.string_or("kind");
    auto child_scope = scope;
    if (kind == "NamespaceDecl") {
        child_scope = join_name(scope, anonymous_scope_name(node));
    } else if (is_record_kind(kind) && !node.bool_or("isImplicit")) {
        const auto name = node.string_or("name");
        if (!name.empty())
            child_scope = join_name(scope, name);
    }

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
        auto key = !mangled.empty() ? mangled : qualified + "|" + type_string(node);
        if (internal)
            key += "@" + state.command.file.string();
        const bool project_owned = is_project_file(state, file);
        if (!ast_id.empty() && !key.empty() && project_owned) {
            const auto line = node_line(node);
            Symbol symbol{
                .key = std::move(key),
                .name = name,
                .qualified_name = qualified,
                .class_name = symbol_kind(kind) == SymbolKind::Function ? "" : owner_scope,
                .signature = type_string(node),
                .file = file,
                .line = line,
                .end_line = node_end_line(node, line),
                .kind = symbol_kind(kind),
                // A FunctionDecl nested in a FunctionTemplateDecl describes the pattern and has no
                // mangled identity. Concrete specializations are indexed separately. Reporting the
                // pattern itself produces a false dead finding even when its instantiations are
                // live.
                .defined = has_body(node) && !mangled.empty(),
                .project_owned = true,
                .internal_linkage = internal,
                .is_virtual = node.bool_or("virtual") || has_override_attribute(node),
            };
            const auto symbol_id = state.graph.add_or_merge_symbol(std::move(symbol));
            state.declarations[ast_id] = symbol_id;
            if (name == "main" && has_body(node))
                state.graph.add_root(symbol_id, "application entry point");
        }
    }

    for (const auto& child : children(node)) {
        collect_declarations(child, state, child_scope, file);
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

std::vector<SymbolId> resolve_references(const Value& expression,
                                         const TranslationUnitState& state) {
    std::vector<std::string> ast_ids;
    collect_reference_ids(expression, ast_ids);
    std::vector<SymbolId> result;
    for (const auto& ast_id : ast_ids) {
        if (const auto found = state.declarations.find(ast_id);
            found != state.declarations.end() &&
            std::ranges::find(result, found->second) == result.end()) {
            result.push_back(found->second);
        }
    }
    return result;
}

bool is_call_expression(std::string_view kind) {
    return kind == "CallExpr" || kind == "CXXMemberCallExpr" || kind == "CXXOperatorCallExpr" ||
           kind == "CUDAKernelCallExpr";
}

void add_use(TranslationUnitState& state, std::optional<SymbolId> caller, SymbolId target,
             EdgeKind kind, bool global_initializer) {
    if (caller.has_value())
        state.graph.add_edge(*caller, target, kind);
    else if (global_initializer)
        state.graph.add_root(target, "global initializer");
}

void add_construction_edges(TranslationUnitState& state, std::optional<SymbolId> caller,
                            const Value& node, bool global_initializer) {
    const auto constructed_type = normalize_type(desugared_type_string(node));
    if (constructed_type.empty())
        return;
    std::vector<std::string> constructed_classes{constructed_type};
    std::unordered_set<std::string> visited;
    for (std::size_t index = 0; index < constructed_classes.size(); ++index) {
        const auto current = constructed_classes[index];
        for (const auto& [record_name, record] : state.records) {
            if (!names_equivalent(record_name, current) || !visited.insert(record_name).second)
                continue;
            constructed_classes.push_back(record_name);
            constructed_classes.insert(constructed_classes.end(), record.bases.begin(),
                                       record.bases.end());
        }
    }
    for (SymbolId id = 0; id < state.graph.symbols().size(); ++id) {
        const auto& symbol = state.graph.symbols()[id];
        if ((symbol.kind == SymbolKind::Constructor || symbol.kind == SymbolKind::Destructor) &&
            std::ranges::any_of(constructed_classes, [&](const std::string& class_name) {
                return names_equivalent(symbol.class_name, normalize_type(class_name));
            })) {
            add_use(state, caller, id, EdgeKind::Constructs, global_initializer);
        }
    }
}

void collect_uses(const Value& node, TranslationUnitState& state, std::optional<SymbolId> caller,
                  bool global_initializer, bool callee_position, bool top_level) {
    const auto kind = node.string_or("kind");
    if (is_function_kind(kind)) {
        if (const auto declaration = state.declarations.find(node.string_or("id"));
            declaration != state.declarations.end()) {
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

    if (is_call_expression(kind)) {
        const auto& inner = children(node);
        if (!inner.empty()) {
            const auto targets = resolve_references(inner.front(), state);
            for (const auto target : targets) {
                add_use(state, caller, target, EdgeKind::DirectCall, global_initializer);
            }
            collect_uses(inner.front(), state, caller, global_initializer, true, false);
            for (std::size_t index = 1; index < inner.size(); ++index) {
                collect_uses(inner[index], state, caller, global_initializer, false, false);
            }
        }
        return;
    }

    if (kind == "CXXConstructExpr" || kind == "CXXTemporaryObjectExpr") {
        add_construction_edges(state, caller, node, global_initializer);
    }

    if (!callee_position && (kind == "DeclRefExpr" || kind == "MemberExpr")) {
        for (const auto target : resolve_references(node, state)) {
            state.graph.symbols()[target].address_taken = true;
            if (caller.has_value())
                state.graph.add_edge(*caller, target, EdgeKind::AddressTaken);
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
                        graph.add_edge(base_id, derived, EdgeKind::VirtualDispatch);
                    }
                }
            }
        }
    }
}

} // namespace

ClangAstIndexer::ClangAstIndexer(IndexOptions options) : options_(std::move(options)) {
    options_.project_root = std::filesystem::absolute(options_.project_root).lexically_normal();
    for (auto& excluded : options_.excluded_paths)
        excluded = std::filesystem::absolute(excluded).lexically_normal();
}

IndexResult ClangAstIndexer::index(const std::vector<CompileCommand>& commands) const {
    if (commands.empty())
        throw std::runtime_error("compilation database contains no commands");
    IndexResult result;
    std::unordered_map<std::string, RecordInfo> records;

    for (const auto& command : commands) {
        const auto invocation = ast_command(command, options_);
        const auto process = run_process(invocation, command.directory);
        if (process.exit_code != 0) {
            throw std::runtime_error("Clang AST indexing failed for " + command.file.string() +
                                     " (exit " + std::to_string(process.exit_code) + "):\n" +
                                     concise_diagnostic(process.standard_error));
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
            throw std::runtime_error("invalid Clang AST JSON for " + command.file.string() + ": " +
                                     error.what());
        }
        TranslationUnitState state{command, options_, result.graph, {}, {}, records};
        for (const auto& ast : ast_documents)
            collect_contexts(ast, state, "", command.file);
        for (const auto& ast : ast_documents)
            collect_declarations(ast, state, "", command.file);
        for (const auto& ast : ast_documents)
            collect_uses(ast, state, std::nullopt, false, false, false);
        ++result.translation_units;
    }

    add_virtual_dispatch_edges(result.graph, records);

    for (const auto& requested : options_.manual_roots) {
        bool matched = false;
        for (SymbolId id = 0; id < result.graph.symbols().size(); ++id) {
            const auto& symbol = result.graph.symbols()[id];
            if (symbol.qualified_name == requested || symbol.key == requested) {
                result.graph.add_root(id, "configured root");
                matched = true;
            }
        }
        if (!matched)
            result.diagnostics.push_back("configured root did not match a symbol: " + requested);
    }
    if (!options_.ast_filter.empty())
        result.diagnostics.push_back("Clang AST name filter active: " + options_.ast_filter);
    if (result.graph.roots().empty()) {
        throw std::runtime_error(
            "no application roots found; include main() or provide a matching --root symbol");
    }
    return result;
}

} // namespace cxx_dead
