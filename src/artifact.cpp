#include "cxx_dead/artifact.h"

#include "cxx_dead/json.h"

#include <algorithm>
#include <cmath>
#include <istream>
#include <iterator>
#include <limits>
#include <ostream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace cxx_dead {

namespace {

void write_source_point(std::ostream& output, const SourcePoint& point) {
    output << "{\"file\": \"" << json::escape(point.file.generic_string())
           << "\", \"line\": " << point.line << ", \"column\": " << point.column
           << ", \"offset\": " << point.offset << ", \"token_length\": " << point.token_length
           << '}';
}

void write_source_extent(std::ostream& output, const SourceExtent& extent) {
    output << "{\"location\": ";
    write_source_point(output, extent.location);
    output << ", \"range\": {\"begin\": ";
    write_source_point(output, extent.begin);
    output << ", \"end\": ";
    write_source_point(output, extent.end);
    output << "}}";
}

void write_source(std::ostream& output, const SymbolSource& source) {
    output << "{\"spelling\": ";
    write_source_extent(output, source.spelling);
    output << ", \"expansion\": ";
    if (source.expansion.has_value())
        write_source_extent(output, *source.expansion);
    else
        output << "null";
    output << '}';
}

void write_evidence(std::ostream& output, const Evidence& evidence) {
    output << "{\"provider\": \"" << json::escape(evidence.provider) << "\", \"reason\": \""
           << json::escape(evidence.reason) << "\"}";
}

std::vector<SymbolId> sorted_symbols(const Graph& graph) {
    std::vector<SymbolId> result(graph.symbols().size());
    for (SymbolId id = 0; id < result.size(); ++id)
        result[id] = id;
    std::ranges::sort(result, [&](SymbolId left, SymbolId right) {
        return graph.symbols()[left].key < graph.symbols()[right].key;
    });
    return result;
}

} // namespace

void write_graph_artifact(std::ostream& output, const Graph& graph,
                          const GraphArtifactMetadata& metadata,
                          const std::vector<std::string>& diagnostics) {
    if (metadata.configuration_id.empty())
        throw std::invalid_argument("graph artifact configuration identity cannot be empty");
    for (const auto& symbol : graph.symbols()) {
        if (symbol.identity.configuration_id != metadata.configuration_id) {
            throw std::invalid_argument(
                "graph artifact configuration does not match symbol identity: " + symbol.key);
        }
    }
    const auto symbol_order = sorted_symbols(graph);
    std::vector<const Edge*> edges;
    edges.reserve(graph.edges().size());
    for (const auto& edge : graph.edges())
        edges.push_back(&edge);
    std::ranges::sort(edges, [&](const Edge* left, const Edge* right) {
        return std::tuple{graph.symbols()[left->from].key, graph.symbols()[left->to].key,
                          left->kind, left->evidence.provider, left->evidence.reason} <
               std::tuple{graph.symbols()[right->from].key, graph.symbols()[right->to].key,
                          right->kind, right->evidence.provider, right->evidence.reason};
    });
    std::vector<const Root*> roots;
    roots.reserve(graph.roots().size());
    for (const auto& root : graph.roots())
        roots.push_back(&root);
    std::ranges::sort(roots, [&](const Root* left, const Root* right) {
        return std::tuple{graph.symbols()[left->symbol].key, left->kind, left->evidence.provider,
                          left->evidence.reason} < std::tuple{graph.symbols()[right->symbol].key,
                                                              right->kind, right->evidence.provider,
                                                              right->evidence.reason};
    });
    std::vector<const Escape*> escapes;
    escapes.reserve(graph.escapes().size());
    for (const auto& escape : graph.escapes())
        escapes.push_back(&escape);
    std::ranges::sort(escapes, [&](const Escape* left, const Escape* right) {
        const auto left_from = left->from.has_value() ? graph.symbols()[*left->from].key : "";
        const auto right_from = right->from.has_value() ? graph.symbols()[*right->from].key : "";
        return std::tuple{graph.symbols()[left->symbol].key, left_from, left->kind,
                          left->evidence.provider, left->evidence.reason} <
               std::tuple{graph.symbols()[right->symbol].key, right_from, right->kind,
                          right->evidence.provider, right->evidence.reason};
    });
    std::vector<const Suppression*> suppressions;
    suppressions.reserve(graph.suppressions().size());
    for (const auto& suppression : graph.suppressions())
        suppressions.push_back(&suppression);
    std::ranges::sort(suppressions, [&](const Suppression* left, const Suppression* right) {
        return std::tuple{graph.symbols()[left->symbol].key, left->evidence.provider,
                          left->evidence.reason} < std::tuple{graph.symbols()[right->symbol].key,
                                                              right->evidence.provider,
                                                              right->evidence.reason};
    });
    auto sorted_diagnostics = diagnostics;
    std::ranges::sort(sorted_diagnostics);
    const auto duplicate = std::ranges::unique(sorted_diagnostics);
    sorted_diagnostics.erase(duplicate.begin(), duplicate.end());

    output << "{\n"
           << "  \"artifact_schema_version\": " << graph_artifact_schema_version << ",\n"
           << "  \"identity_schema_version\": " << symbol_identity_schema_version << ",\n"
           << "  \"configuration_id\": \"" << json::escape(metadata.configuration_id) << "\",\n"
           << "  \"analysis_context\": {\"configuration\": \""
           << json::escape(metadata.configuration) << "\", \"target_id\": ";
    if (metadata.target_id.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_id) << '"';
    output << ", \"target_name\": ";
    if (metadata.target_name.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_name) << '"';
    output << ", \"target_kind\": ";
    if (metadata.target_kind.empty())
        output << "null";
    else
        output << '"' << json::escape(metadata.target_kind) << '"';
    output << ", \"closure_targets\": [";
    for (std::size_t index = 0; index < metadata.closure_targets.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << '"' << json::escape(metadata.closure_targets[index]) << '"';
    }
    output << "]},\n"
           << "  \"frontend\": \"" << to_string(metadata.frontend) << "\",\n"
           << "  \"translation_units\": " << metadata.translation_units << ",\n"
           << "  \"symbols\": [";
    for (std::size_t index = 0; index < symbol_order.size(); ++index) {
        const auto& symbol = graph.symbols()[symbol_order[index]];
        output << (index == 0 ? "\n" : ",\n") << "    {\n"
               << "      \"id\": \"" << json::escape(symbol.key) << "\",\n"
               << "      \"identity\": {\"quality\": \"" << to_string(symbol.identity.quality)
               << "\", \"configuration_id\": \"" << json::escape(symbol.identity.configuration_id)
               << "\", \"usr\": \"" << json::escape(symbol.identity.usr)
               << "\", \"linkage_name\": \"" << json::escape(symbol.identity.linkage_name)
               << "\", \"translation_unit\": \"" << json::escape(symbol.identity.translation_unit)
               << "\", \"fallback_anchor\": \"" << json::escape(symbol.identity.fallback_anchor)
               << "\"},\n"
               << "      \"name\": \"" << json::escape(symbol.name) << "\",\n"
               << "      \"qualified_name\": \"" << json::escape(symbol.qualified_name) << "\",\n"
               << "      \"class_name\": \"" << json::escape(symbol.class_name) << "\",\n"
               << "      \"signature\": \"" << json::escape(symbol.signature) << "\",\n"
               << "      \"kind\": \"" << to_string(symbol.kind) << "\",\n"
               << "      \"scope\": \"" << to_string(symbol.scope) << "\",\n"
               << "      \"defined\": " << (symbol.defined ? "true" : "false") << ",\n"
               << "      \"internal_linkage\": " << (symbol.internal_linkage ? "true" : "false")
               << ",\n"
               << "      \"virtual\": " << (symbol.is_virtual ? "true" : "false") << ",\n"
               << "      \"source\": ";
        write_source(output, symbol.source);
        output << "\n    }";
    }
    if (!symbol_order.empty())
        output << '\n';
    output << "  ],\n  \"edges\": [";
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto& edge = *edges[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"from\": \""
               << json::escape(graph.symbols()[edge.from].key) << "\", \"to\": \""
               << json::escape(graph.symbols()[edge.to].key) << "\", \"kind\": \""
               << to_string(edge.kind) << "\", \"evidence\": ";
        write_evidence(output, edge.evidence);
        output << '}';
    }
    if (!edges.empty())
        output << '\n';
    output << "  ],\n  \"roots\": [";
    for (std::size_t index = 0; index < roots.size(); ++index) {
        const auto& root = *roots[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"symbol\": \""
               << json::escape(graph.symbols()[root.symbol].key) << "\", \"kind\": \""
               << to_string(root.kind) << "\", \"evidence\": ";
        write_evidence(output, root.evidence);
        output << '}';
    }
    if (!roots.empty())
        output << '\n';
    output << "  ],\n  \"escapes\": [";
    for (std::size_t index = 0; index < escapes.size(); ++index) {
        const auto& escape = *escapes[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"symbol\": \""
               << json::escape(graph.symbols()[escape.symbol].key) << "\", \"from\": ";
        if (escape.from.has_value())
            output << '"' << json::escape(graph.symbols()[*escape.from].key) << '"';
        else
            output << "null";
        output << ", \"kind\": \"" << to_string(escape.kind) << "\", \"evidence\": ";
        write_evidence(output, escape.evidence);
        output << '}';
    }
    if (!escapes.empty())
        output << '\n';
    output << "  ],\n  \"suppressions\": [";
    for (std::size_t index = 0; index < suppressions.size(); ++index) {
        const auto& suppression = *suppressions[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"symbol\": \""
               << json::escape(graph.symbols()[suppression.symbol].key) << "\", \"evidence\": ";
        write_evidence(output, suppression.evidence);
        output << '}';
    }
    if (!suppressions.empty())
        output << '\n';
    output << "  ],\n  \"diagnostics\": [";
    for (std::size_t index = 0; index < sorted_diagnostics.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n") << "    \"" << json::escape(sorted_diagnostics[index])
               << '"';
    }
    if (!sorted_diagnostics.empty())
        output << '\n';
    output << "  ]\n}\n";
}

namespace {

using JsonObject = json::Value::Object;

[[noreturn]] void invalid_artifact(std::string_view context, std::string_view message) {
    throw std::runtime_error("invalid graph artifact " + std::string(context) + ": " +
                             std::string(message));
}

const JsonObject& require_object(const json::Value& value, std::string_view context) {
    if (!value.is_object())
        invalid_artifact(context, "must be an object");
    return value.as_object();
}

void require_keys(const json::Value& value, std::initializer_list<std::string_view> required,
                  std::initializer_list<std::string_view> optional, std::string_view context) {
    const auto& object = require_object(value, context);
    for (const auto key : required) {
        if (!object.contains(key))
            invalid_artifact(context, "requires '" + std::string(key) + "'");
    }
    for (const auto& [key, unused] : object) {
        const auto allowed = std::ranges::find(required, key) != required.end() ||
                             std::ranges::find(optional, key) != optional.end();
        if (!allowed)
            invalid_artifact(context, "contains unknown key '" + key + "'");
    }
}

const json::Value& require_value(const json::Value& value, std::string_view key,
                                 std::string_view context) {
    const auto& object = require_object(value, context);
    const auto found = object.find(key);
    if (found == object.end())
        invalid_artifact(context, "requires '" + std::string(key) + "'");
    return found->second;
}

std::string require_string(const json::Value& value, std::string_view key, std::string_view context,
                           bool allow_empty = true) {
    const auto& field = require_value(value, key, context);
    if (!field.is_string())
        invalid_artifact(context, "'" + std::string(key) + "' must be a string");
    if (!allow_empty && field.as_string().empty())
        invalid_artifact(context, "'" + std::string(key) + "' cannot be empty");
    return field.as_string();
}

std::string nullable_string(const json::Value& value, std::string_view key,
                            std::string_view context) {
    const auto& field = require_value(value, key, context);
    if (field.is_null())
        return {};
    if (!field.is_string() || field.as_string().empty())
        invalid_artifact(context, "'" + std::string(key) + "' must be null or a non-empty string");
    return field.as_string();
}

bool require_bool(const json::Value& value, std::string_view key, std::string_view context) {
    const auto& field = require_value(value, key, context);
    if (!field.is_bool())
        invalid_artifact(context, "'" + std::string(key) + "' must be a boolean");
    return field.as_bool();
}

std::size_t require_size(const json::Value& value, std::string_view key, std::string_view context) {
    const auto& field = require_value(value, key, context);
    constexpr double largest_exact_json_integer = 9007199254740991.0;
    if (!field.is_number() || field.as_number() < 0.0 ||
        std::floor(field.as_number()) != field.as_number() ||
        field.as_number() > largest_exact_json_integer ||
        field.as_number() > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        invalid_artifact(context, "'" + std::string(key) + "' must be a non-negative integer");
    }
    return static_cast<std::size_t>(field.as_number());
}

const json::Value::Array& require_array(const json::Value& value, std::string_view key,
                                        std::string_view context) {
    const auto& field = require_value(value, key, context);
    if (!field.is_array())
        invalid_artifact(context, "'" + std::string(key) + "' must be an array");
    return field.as_array();
}

SourcePoint parse_source_point(const json::Value& value, std::string_view context) {
    require_keys(value, {"file", "line", "column", "offset", "token_length"}, {}, context);
    return {
        .file = require_string(value, "file", context),
        .line = require_size(value, "line", context),
        .column = require_size(value, "column", context),
        .offset = require_size(value, "offset", context),
        .token_length = require_size(value, "token_length", context),
    };
}

SourceExtent parse_source_extent(const json::Value& value, std::string_view context) {
    require_keys(value, {"location", "range"}, {}, context);
    const auto& range = require_value(value, "range", context);
    require_keys(range, {"begin", "end"}, {}, std::string(context) + ".range");
    return {
        .location = parse_source_point(require_value(value, "location", context),
                                       std::string(context) + ".location"),
        .begin = parse_source_point(require_value(range, "begin", context),
                                    std::string(context) + ".range.begin"),
        .end = parse_source_point(require_value(range, "end", context),
                                  std::string(context) + ".range.end"),
    };
}

SymbolSource parse_source(const json::Value& value, std::string_view context) {
    require_keys(value, {"spelling", "expansion"}, {}, context);
    SymbolSource result{
        .spelling = parse_source_extent(require_value(value, "spelling", context),
                                        std::string(context) + ".spelling"),
        .expansion = std::nullopt,
    };
    const auto& expansion = require_value(value, "expansion", context);
    if (!expansion.is_null())
        result.expansion = parse_source_extent(expansion, std::string(context) + ".expansion");
    return result;
}

Evidence parse_evidence(const json::Value& value, std::string_view context) {
    require_keys(value, {"provider", "reason"}, {}, context);
    return {.provider = require_string(value, "provider", context),
            .reason = require_string(value, "reason", context)};
}

template <typename Enum>
Enum parse_enum(std::string_view value,
                std::initializer_list<std::pair<std::string_view, Enum>> map,
                std::string_view context) {
    const auto found =
        std::ranges::find_if(map, [&](const auto& item) { return item.first == value; });
    if (found == map.end())
        invalid_artifact(context, "contains unsupported value '" + std::string(value) + "'");
    return found->second;
}

IndexFrontend parse_frontend(std::string_view value, std::string_view context) {
    return parse_enum<IndexFrontend>(
        value, {{"ast-json", IndexFrontend::AstJson}, {"libtooling", IndexFrontend::LibTooling}},
        context);
}

Symbol parse_symbol(const json::Value& value, std::string_view context) {
    require_keys(value,
                 {"id", "identity", "name", "qualified_name", "class_name", "signature", "kind",
                  "scope", "defined", "internal_linkage", "virtual", "source"},
                 {}, context);
    const auto& identity_value = require_value(value, "identity", context);
    require_keys(identity_value,
                 {"quality", "configuration_id", "usr", "linkage_name", "translation_unit",
                  "fallback_anchor"},
                 {}, std::string(context) + ".identity");
    const auto identity_context = std::string(context) + ".identity";
    SymbolIdentity identity{
        .configuration_id =
            require_string(identity_value, "configuration_id", identity_context, false),
        .usr = require_string(identity_value, "usr", identity_context),
        .linkage_name = require_string(identity_value, "linkage_name", identity_context),
        .translation_unit = require_string(identity_value, "translation_unit", identity_context),
        .fallback_anchor = require_string(identity_value, "fallback_anchor", identity_context),
        .quality = parse_enum<IdentityQuality>(
            require_string(identity_value, "quality", identity_context, false),
            {{"stable", IdentityQuality::Stable}, {"fallback", IdentityQuality::Fallback}},
            identity_context + ".quality"),
    };
    Symbol result{
        .key = require_string(value, "id", context, false),
        .identity = std::move(identity),
        .name = require_string(value, "name", context),
        .qualified_name = require_string(value, "qualified_name", context),
        .class_name = require_string(value, "class_name", context),
        .signature = require_string(value, "signature", context),
        .source =
            parse_source(require_value(value, "source", context), std::string(context) + ".source"),
        .kind = parse_enum<SymbolKind>(require_string(value, "kind", context, false),
                                       {{"function", SymbolKind::Function},
                                        {"method", SymbolKind::Method},
                                        {"constructor", SymbolKind::Constructor},
                                        {"destructor", SymbolKind::Destructor},
                                        {"synthetic", SymbolKind::Synthetic}},
                                       std::string(context) + ".kind"),
        .scope = parse_enum<SymbolScope>(require_string(value, "scope", context, false),
                                         {{"reportable", SymbolScope::Reportable},
                                          {"indexed", SymbolScope::Indexed},
                                          {"external_opaque", SymbolScope::ExternalOpaque}},
                                         std::string(context) + ".scope"),
        .defined = require_bool(value, "defined", context),
        .internal_linkage = require_bool(value, "internal_linkage", context),
        .is_virtual = require_bool(value, "virtual", context),
    };
    SymbolIdentity validated_identity;
    try {
        validated_identity = make_symbol_identity(
            result.identity.configuration_id, result.identity.usr, result.identity.linkage_name,
            result.identity.translation_unit, result.identity.fallback_anchor);
    } catch (const std::exception& error) {
        invalid_artifact(context, error.what());
    }
    if (validated_identity.quality != result.identity.quality)
        invalid_artifact(context, "identity quality does not match its identity fields");
    if (stable_symbol_key(result.identity) != result.key)
        invalid_artifact(context, "symbol id does not match its identity fields");
    return result;
}

} // namespace

GraphArtifact read_graph_artifact(std::istream& input) {
    const std::string document((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail())
        throw std::runtime_error("could not read graph artifact");
    json::Value root;
    try {
        root = json::parse(document);
    } catch (const json::ParseError& error) {
        throw std::runtime_error("invalid graph artifact JSON: " + std::string(error.what()));
    }
    require_keys(root,
                 {"artifact_schema_version", "identity_schema_version", "configuration_id",
                  "analysis_context", "frontend", "translation_units", "symbols", "edges", "roots",
                  "escapes", "suppressions", "diagnostics"},
                 {}, "document");
    if (require_size(root, "artifact_schema_version", "document") !=
        static_cast<std::size_t>(graph_artifact_schema_version)) {
        invalid_artifact("document", "unsupported artifact_schema_version");
    }
    if (require_size(root, "identity_schema_version", "document") !=
        static_cast<std::size_t>(symbol_identity_schema_version)) {
        invalid_artifact("document", "unsupported identity_schema_version");
    }

    GraphArtifact result;
    result.metadata.configuration_id = require_string(root, "configuration_id", "document", false);
    result.metadata.frontend =
        parse_frontend(require_string(root, "frontend", "document", false), "document.frontend");
    result.metadata.translation_units = require_size(root, "translation_units", "document");
    const auto& context = require_value(root, "analysis_context", "document");
    require_keys(context,
                 {"configuration", "target_id", "target_name", "target_kind", "closure_targets"},
                 {}, "document.analysis_context");
    result.metadata.configuration = require_string(context, "configuration", "analysis_context");
    result.metadata.target_id = nullable_string(context, "target_id", "analysis_context");
    result.metadata.target_name = nullable_string(context, "target_name", "analysis_context");
    result.metadata.target_kind = nullable_string(context, "target_kind", "analysis_context");
    const auto target_fields = static_cast<int>(!result.metadata.target_id.empty()) +
                               static_cast<int>(!result.metadata.target_name.empty()) +
                               static_cast<int>(!result.metadata.target_kind.empty());
    if (target_fields != 0 && target_fields != 3)
        invalid_artifact("analysis_context", "target id, name, and kind must appear together");
    if (!result.metadata.target_kind.empty() &&
        !std::set<std::string_view>{"executable", "static_library", "shared_library",
                                    "module_library", "object_library", "interface_library",
                                    "utility"}
             .contains(result.metadata.target_kind)) {
        invalid_artifact("analysis_context.target_kind", "contains an unsupported target kind");
    }
    for (const auto& value : require_array(context, "closure_targets", "analysis_context")) {
        if (!value.is_string() || value.as_string().empty())
            invalid_artifact("analysis_context.closure_targets",
                             "entries must be non-empty strings");
        result.metadata.closure_targets.push_back(value.as_string());
    }

    std::unordered_map<std::string, SymbolId> ids;
    const auto& symbols = require_array(root, "symbols", "document");
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        const auto context_name = "document.symbols[" + std::to_string(index) + "]";
        auto symbol = parse_symbol(symbols[index], context_name);
        if (symbol.identity.configuration_id != result.metadata.configuration_id)
            invalid_artifact(context_name, "configuration identity does not match the artifact");
        if (ids.contains(symbol.key))
            invalid_artifact(context_name, "duplicate symbol id");
        const auto key = symbol.key;
        ids.emplace(key, result.graph.add_or_merge_symbol(std::move(symbol)));
    }
    const auto symbol_id = [&](std::string_view key, std::string_view context_name) {
        const auto found = ids.find(std::string(key));
        if (found == ids.end())
            invalid_artifact(context_name, "references unknown symbol '" + std::string(key) + "'");
        return found->second;
    };

    const auto& edges = require_array(root, "edges", "document");
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto context_name = "document.edges[" + std::to_string(index) + "]";
        const auto& item = edges[index];
        require_keys(item, {"from", "to", "kind", "evidence"}, {}, context_name);
        result.graph.add_edge(
            symbol_id(require_string(item, "from", context_name, false), context_name + ".from"),
            symbol_id(require_string(item, "to", context_name, false), context_name + ".to"),
            parse_enum<EdgeKind>(require_string(item, "kind", context_name, false),
                                 {{"direct_call", EdgeKind::DirectCall},
                                  {"constructs", EdgeKind::Constructs},
                                  {"virtual_dispatch", EdgeKind::VirtualDispatch},
                                  {"callback_registration", EdgeKind::CallbackRegistration},
                                  {"provider", EdgeKind::Provider}},
                                 context_name + ".kind"),
            parse_evidence(require_value(item, "evidence", context_name),
                           context_name + ".evidence"));
    }
    const auto& roots = require_array(root, "roots", "document");
    for (std::size_t index = 0; index < roots.size(); ++index) {
        const auto context_name = "document.roots[" + std::to_string(index) + "]";
        const auto& item = roots[index];
        require_keys(item, {"symbol", "kind", "evidence"}, {}, context_name);
        result.graph.add_root(
            symbol_id(require_string(item, "symbol", context_name, false),
                      context_name + ".symbol"),
            parse_enum<RootKind>(require_string(item, "kind", context_name, false),
                                 {{"application_entry_point", RootKind::ApplicationEntryPoint},
                                  {"global_initializer", RootKind::GlobalInitializer},
                                  {"manual", RootKind::Manual},
                                  {"callback_registration", RootKind::CallbackRegistration},
                                  {"provider", RootKind::Provider},
                                  {"public_api", RootKind::PublicApi}},
                                 context_name + ".kind"),
            parse_evidence(require_value(item, "evidence", context_name),
                           context_name + ".evidence"));
    }
    const auto& escapes = require_array(root, "escapes", "document");
    for (std::size_t index = 0; index < escapes.size(); ++index) {
        const auto context_name = "document.escapes[" + std::to_string(index) + "]";
        const auto& item = escapes[index];
        require_keys(item, {"symbol", "from", "kind", "evidence"}, {}, context_name);
        const auto& from = require_value(item, "from", context_name);
        std::optional<SymbolId> from_id;
        if (!from.is_null()) {
            if (!from.is_string() || from.as_string().empty())
                invalid_artifact(context_name + ".from", "must be null or a symbol id");
            from_id = symbol_id(from.as_string(), context_name + ".from");
        }
        result.graph.add_escape(
            symbol_id(require_string(item, "symbol", context_name, false),
                      context_name + ".symbol"),
            parse_enum<EscapeKind>(require_string(item, "kind", context_name, false),
                                   {{"address_taken", EscapeKind::AddressTaken},
                                    {"callable_object", EscapeKind::CallableObject},
                                    {"provider", EscapeKind::Provider}},
                                   context_name + ".kind"),
            parse_evidence(require_value(item, "evidence", context_name),
                           context_name + ".evidence"),
            from_id);
    }
    const auto& suppressions = require_array(root, "suppressions", "document");
    for (std::size_t index = 0; index < suppressions.size(); ++index) {
        const auto context_name = "document.suppressions[" + std::to_string(index) + "]";
        const auto& item = suppressions[index];
        require_keys(item, {"symbol", "evidence"}, {}, context_name);
        result.graph.add_suppression(symbol_id(require_string(item, "symbol", context_name, false),
                                               context_name + ".symbol"),
                                     parse_evidence(require_value(item, "evidence", context_name),
                                                    context_name + ".evidence"));
    }
    for (const auto& diagnostic : require_array(root, "diagnostics", "document")) {
        if (!diagnostic.is_string())
            invalid_artifact("document.diagnostics", "entries must be strings");
        result.diagnostics.push_back(diagnostic.as_string());
    }
    result.graph.canonicalize();
    return result;
}

} // namespace cxx_dead
