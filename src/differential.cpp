#include "cxx_dead/differential.h"

#include "cxx_dead/json.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace cxx_dead {

namespace {

using KeySet = std::set<std::string, std::less<>>;

std::string policy_context(const std::filesystem::path& path, std::string_view context) {
    return path.string() + ": " + std::string(context);
}

void require_policy_mapping(const YAML::Node& node, const KeySet& allowed,
                            const std::filesystem::path& path, std::string_view context) {
    if (!node.IsMap())
        throw std::runtime_error(policy_context(path, context) + " must be a mapping");
    KeySet seen;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar())
            throw std::runtime_error(policy_context(path, context) + " contains a non-scalar key");
        const auto key = entry.first.as<std::string>();
        if (!allowed.contains(key))
            throw std::runtime_error(policy_context(path, context) + " contains unknown key '" +
                                     key + "'");
        if (!seen.insert(key).second)
            throw std::runtime_error(policy_context(path, context) + " contains duplicate key '" +
                                     key + "'");
    }
}

template <typename Enum, typename Parse>
std::vector<Enum> parse_policy_enum_list(const YAML::Node& root, std::string_view key,
                                         const std::filesystem::path& path, Parse&& parse) {
    const auto sequence = root[std::string(key)];
    if (!sequence)
        return {};
    if (!sequence.IsSequence() || sequence.size() == 0U)
        throw std::runtime_error(policy_context(path, key) + " must be a non-empty sequence");
    std::vector<Enum> result;
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        if (!sequence[index].IsScalar()) {
            throw std::runtime_error(policy_context(
                path, std::string(key) + "[" + std::to_string(index) + "] must be a string"));
        }
        const auto value = sequence[index].as<std::string>();
        const auto parsed = parse(value);
        if (std::ranges::find(result, parsed) != result.end()) {
            throw std::runtime_error(policy_context(path, key) + " contains duplicate value '" +
                                     value + "'");
        }
        result.push_back(parsed);
    }
    return result;
}

Classification parse_classification(std::string_view value, const std::filesystem::path& path) {
    if (value == "dead")
        return Classification::Dead;
    if (value == "likely_dead")
        return Classification::LikelyDead;
    if (value == "possibly_dead")
        return Classification::PossiblyDead;
    if (value == "dynamically_referenced")
        return Classification::DynamicallyReferenced;
    throw std::runtime_error(policy_context(path, "classifications") +
                             " contains unsupported value '" + std::string(value) + "'");
}

ChangeKind parse_policy_change(std::string_view value, const std::filesystem::path& path) {
    if (value == "new_symbol")
        return ChangeKind::NewSymbol;
    if (value == "newly_unreachable")
        return ChangeKind::NewlyUnreachable;
    throw std::runtime_error(policy_context(path, "changes") + " contains unsupported value '" +
                             std::string(value) + "'");
}

bool eligible_symbol(const Symbol& symbol) {
    return symbol.defined && is_reportable(symbol.scope) && symbol.kind != SymbolKind::Synthetic;
}

struct FindingLookup {
    const Finding* finding{nullptr};
    const SuppressedFinding* suppressed{nullptr};
};

FindingLookup find_finding(const AnalysisReport& report, SymbolId id) {
    const auto finding = std::ranges::find_if(
        report.findings, [=](const Finding& item) { return item.symbol == id; });
    if (finding != report.findings.end())
        return {.finding = &*finding};
    const auto suppressed =
        std::ranges::find_if(report.suppressed_findings, [=](const SuppressedFinding& item) {
            return item.finding.symbol == id;
        });
    if (suppressed != report.suppressed_findings.end())
        return {.finding = &suppressed->finding, .suppressed = &*suppressed};
    return {};
}

DifferentialSymbolState symbol_state(const Graph& graph, const ReachabilityResult& reachability,
                                     const AnalysisReport& report, std::optional<SymbolId> id) {
    if (!id.has_value())
        return {};
    DifferentialSymbolState result;
    result.present = true;
    result.reachable = reachability.reachable[*id];
    const auto lookup = find_finding(report, *id);
    if (lookup.finding == nullptr)
        return result;
    result.classification = lookup.finding->classification;
    result.confidence = lookup.finding->confidence;
    result.suppressed = lookup.suppressed != nullptr;
    if (lookup.suppressed != nullptr)
        result.suppressions = lookup.suppressed->suppressions;
    for (const auto& item : lookup.finding->evidence) {
        result.evidence.push_back({
            .kind = item.kind,
            .evidence = item.evidence,
            .escape_kind = item.escape_kind,
            .from_symbol = item.from.has_value() ? graph.symbols()[*item.from].key : "",
        });
    }
    return result;
}

void require_compatible_context(const GraphArtifactMetadata& baseline,
                                const GraphArtifactMetadata& current) {
    if (baseline.configuration_id != current.configuration_id)
        throw std::runtime_error("baseline/current configuration identities differ");
    if (baseline.frontend != current.frontend)
        throw std::runtime_error("baseline/current index frontends differ");
    if (baseline.target_id != current.target_id || baseline.target_name != current.target_name ||
        baseline.target_kind != current.target_kind) {
        throw std::runtime_error("baseline/current target identities differ");
    }
}

bool policy_matches(const DifferentialPolicy& policy, const GraphArtifactMetadata& current,
                    const DifferentialChange& change) {
    if (std::ranges::find(policy.changes, change.kind) == policy.changes.end() ||
        change.current.suppressed || !change.current.classification.has_value() ||
        std::ranges::find(policy.classifications, *change.current.classification) ==
            policy.classifications.end() ||
        change.current.confidence < policy.minimum_confidence) {
        return false;
    }
    return policy.targets.empty() ||
           std::ranges::find(policy.targets, current.target_name) != policy.targets.end();
}

void write_nullable_string(std::ostream& output, std::string_view value) {
    if (value.empty())
        output << "null";
    else
        output << '"' << json::escape(value) << '"';
}

void write_metadata(std::ostream& output, const GraphArtifactMetadata& metadata) {
    output << "{\"configuration_id\": \"" << json::escape(metadata.configuration_id)
           << "\", \"configuration\": \"" << json::escape(metadata.configuration)
           << "\", \"target_id\": ";
    write_nullable_string(output, metadata.target_id);
    output << ", \"target_name\": ";
    write_nullable_string(output, metadata.target_name);
    output << ", \"target_kind\": ";
    write_nullable_string(output, metadata.target_kind);
    output << ", \"frontend\": \"" << to_string(metadata.frontend)
           << "\", \"translation_units\": " << metadata.translation_units
           << ", \"closure_targets\": [";
    for (std::size_t index = 0; index < metadata.closure_targets.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << '"' << json::escape(metadata.closure_targets[index]) << '"';
    }
    output << "]}";
}

void write_source_point(std::ostream& output, const SourcePoint& point) {
    output << "{\"file\": \"" << json::escape(point.file.generic_string())
           << "\", \"line\": " << point.line << ", \"column\": " << point.column
           << ", \"offset\": " << point.offset << ", \"token_length\": " << point.token_length
           << '}';
}

void write_state(std::ostream& output, const DifferentialSymbolState& state) {
    output << "{\"present\": " << (state.present ? "true" : "false")
           << ", \"reachable\": " << (state.reachable ? "true" : "false")
           << ", \"suppressed\": " << (state.suppressed ? "true" : "false")
           << ", \"classification\": ";
    if (state.classification.has_value())
        output << '"' << to_string(*state.classification) << '"';
    else
        output << "null";
    output << ", \"confidence\": ";
    if (state.classification.has_value())
        output << state.confidence;
    else
        output << "null";
    output << ", \"evidence\": [";
    for (std::size_t index = 0; index < state.evidence.size(); ++index) {
        const auto& item = state.evidence[index];
        if (index != 0)
            output << ", ";
        output << "{\"kind\": \"" << to_string(item.kind) << "\", \"provider\": \""
               << json::escape(item.evidence.provider) << "\", \"reason\": \""
               << json::escape(item.evidence.reason) << "\", \"escape_kind\": ";
        if (item.escape_kind.has_value())
            output << '"' << to_string(*item.escape_kind) << '"';
        else
            output << "null";
        output << ", \"from_symbol\": ";
        write_nullable_string(output, item.from_symbol);
        output << '}';
    }
    output << "], \"suppressions\": [";
    for (std::size_t index = 0; index < state.suppressions.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << "{\"provider\": \"" << json::escape(state.suppressions[index].provider)
               << "\", \"reason\": \"" << json::escape(state.suppressions[index].reason) << "\"}";
    }
    output << "]}";
}

std::string display_name(const Symbol& symbol) {
    return symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
}

std::string sarif_rule(const DifferentialChange& change) {
    return "cxx-dead." + std::string(to_string(change.kind)) + "." +
           std::string(to_string(*change.current.classification));
}

std::string uri_encode(std::string_view path) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const auto raw : path) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 || c == '/' || c == '.' || c == '_' || c == '-' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(digits[c >> 4U]);
            result.push_back(digits[c & 0x0fU]);
        }
    }
    return result;
}

std::string sarif_uri(const std::filesystem::path& file,
                      const std::filesystem::path& project_root) {
    if (file.empty())
        return {};
    auto normalized = file.lexically_normal();
    if (normalized.is_absolute()) {
        const auto relative = normalized.lexically_relative(project_root.lexically_normal());
        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
            normalized = relative;
    }
    return uri_encode(normalized.generic_string());
}

} // namespace

DifferentialPolicy load_differential_policy(const std::filesystem::path& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error(path.string() + ": invalid YAML: " + error.what());
    }
    require_policy_mapping(
        root, {"schema_version", "changes", "classifications", "targets", "minimum_confidence"},
        path, "document");
    const auto version_node = root["schema_version"];
    if (!version_node)
        throw std::runtime_error(path.string() + ": document requires 'schema_version'");
    int version = 0;
    try {
        version = version_node.as<int>();
    } catch (const YAML::Exception&) {
        throw std::runtime_error(path.string() + ": schema_version must be an integer");
    }
    if (version != differential_policy_schema_version) {
        throw std::runtime_error(path.string() + ": unsupported differential schema_version " +
                                 std::to_string(version));
    }

    DifferentialPolicy policy;
    policy.source = std::filesystem::absolute(path).lexically_normal();
    if (root["changes"])
        policy.changes = parse_policy_enum_list<ChangeKind>(
            root, "changes", path, [&](auto value) { return parse_policy_change(value, path); });
    if (root["classifications"]) {
        policy.classifications =
            parse_policy_enum_list<Classification>(root, "classifications", path, [&](auto value) {
                return parse_classification(value, path);
            });
    }
    if (root["targets"]) {
        if (!root["targets"].IsSequence() || root["targets"].size() == 0U)
            throw std::runtime_error(policy_context(path, "targets") +
                                     " must be a non-empty sequence");
        for (std::size_t index = 0; index < root["targets"].size(); ++index) {
            const auto item = root["targets"][index];
            if (!item.IsScalar())
                throw std::runtime_error(policy_context(path, "targets") +
                                         " entries must be strings");
            const auto target = item.as<std::string>();
            if (target.empty())
                throw std::runtime_error(policy_context(path, "targets") +
                                         " entries cannot be empty");
            if (std::ranges::find(policy.targets, target) != policy.targets.end()) {
                throw std::runtime_error(policy_context(path, "targets") +
                                         " contains duplicate value '" + target + "'");
            }
            policy.targets.push_back(target);
        }
    }
    if (root["minimum_confidence"]) {
        try {
            policy.minimum_confidence = root["minimum_confidence"].as<double>();
        } catch (const YAML::Exception&) {
            throw std::runtime_error(policy_context(path, "minimum_confidence") +
                                     " must be a number from 0 to 1");
        }
        if (!std::isfinite(policy.minimum_confidence) || policy.minimum_confidence < 0.0 ||
            policy.minimum_confidence > 1.0) {
            throw std::runtime_error(policy_context(path, "minimum_confidence") +
                                     " must be a number from 0 to 1");
        }
    }
    return policy;
}

DifferentialReport build_differential_report(const GraphArtifact& baseline,
                                             const Graph& current_graph,
                                             const ReachabilityResult& current_reachability,
                                             const AnalysisReport& current_report,
                                             const GraphArtifactMetadata& current_metadata,
                                             const std::optional<DifferentialPolicy>& policy) {
    require_compatible_context(baseline.metadata, current_metadata);
    if (policy.has_value() && !policy->targets.empty()) {
        if (current_metadata.target_name.empty() ||
            std::ranges::find(policy->targets, current_metadata.target_name) ==
                policy->targets.end()) {
            throw std::runtime_error("differential policy does not apply to current target '" +
                                     current_metadata.target_name + "'");
        }
    }

    const auto baseline_reachability = analyze_reachability(baseline.graph);
    const auto baseline_report = build_report(baseline.graph, baseline_reachability);
    std::map<std::string, SymbolId, std::less<>> baseline_symbols;
    std::map<std::string, SymbolId, std::less<>> current_symbols;
    for (SymbolId id = 0; id < baseline.graph.symbols().size(); ++id) {
        if (eligible_symbol(baseline.graph.symbols()[id]))
            baseline_symbols.emplace(baseline.graph.symbols()[id].key, id);
    }
    for (SymbolId id = 0; id < current_graph.symbols().size(); ++id) {
        if (eligible_symbol(current_graph.symbols()[id]))
            current_symbols.emplace(current_graph.symbols()[id].key, id);
    }
    KeySet keys;
    for (const auto& [key, unused] : baseline_symbols)
        keys.insert(key);
    for (const auto& [key, unused] : current_symbols)
        keys.insert(key);

    DifferentialReport result;
    result.baseline = baseline.metadata;
    result.current = current_metadata;
    for (const auto& key : keys) {
        const auto baseline_found = baseline_symbols.find(key);
        const auto current_found = current_symbols.find(key);
        const std::optional<SymbolId> baseline_id =
            baseline_found == baseline_symbols.end()
                ? std::nullopt
                : std::optional<SymbolId>{baseline_found->second};
        const std::optional<SymbolId> current_id =
            current_found == current_symbols.end() ? std::nullopt
                                                   : std::optional<SymbolId>{current_found->second};
        std::optional<ChangeKind> kind;
        if (!baseline_id.has_value())
            kind = ChangeKind::NewSymbol;
        else if (!current_id.has_value())
            kind = ChangeKind::Removed;
        else if (baseline_reachability.reachable[*baseline_id] &&
                 !current_reachability.reachable[*current_id])
            kind = ChangeKind::NewlyUnreachable;
        else if (!baseline_reachability.reachable[*baseline_id] &&
                 current_reachability.reachable[*current_id])
            kind = ChangeKind::BecameReachable;
        if (!kind.has_value())
            continue;

        DifferentialChange change{
            .kind = *kind,
            .symbol = current_id.has_value() ? current_graph.symbols()[*current_id]
                                             : baseline.graph.symbols()[*baseline_id],
            .baseline =
                symbol_state(baseline.graph, baseline_reachability, baseline_report, baseline_id),
            .current =
                symbol_state(current_graph, current_reachability, current_report, current_id),
        };
        if (policy.has_value())
            change.policy_match = policy_matches(*policy, current_metadata, change);
        switch (*kind) {
        case ChangeKind::NewSymbol:
            ++result.new_symbols;
            break;
        case ChangeKind::NewlyUnreachable:
            ++result.newly_unreachable;
            break;
        case ChangeKind::Removed:
            ++result.removed;
            break;
        case ChangeKind::BecameReachable:
            ++result.became_reachable;
            break;
        }
        if (change.policy_match)
            ++result.policy_matches;
        result.changes.push_back(std::move(change));
    }
    return result;
}

std::string_view to_string(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::NewSymbol:
        return "new_symbol";
    case ChangeKind::NewlyUnreachable:
        return "newly_unreachable";
    case ChangeKind::Removed:
        return "removed";
    case ChangeKind::BecameReachable:
        return "became_reachable";
    }
    return "unknown";
}

void write_human_differential_report(std::ostream& output, const DifferentialReport& report) {
    output << "cxx-dead differential reachability report\n\n"
           << "Context: "
           << (report.current.target_name.empty() ? "application" : report.current.target_name)
           << " [" << report.current.configuration_id << "]\n"
           << "Frontend: " << to_string(report.current.frontend) << "\n\n"
           << "SUMMARY\n"
           << "  New symbols:          " << report.new_symbols << '\n'
           << "  Newly unreachable:    " << report.newly_unreachable << '\n'
           << "  Removed:              " << report.removed << '\n'
           << "  Became reachable:     " << report.became_reachable << '\n'
           << "  Policy matches:       " << report.policy_matches << "\n\n";
    if (report.changes.empty()) {
        output << "No reportable reachability changes found.\n";
        return;
    }
    for (const auto& change : report.changes) {
        output << to_string(change.kind) << (change.policy_match ? " [policy_match]" : "") << "\n"
               << "  " << display_name(change.symbol) << " : " << change.symbol.signature << '\n'
               << "  Stable ID: " << change.symbol.key << '\n';
        const auto& location = primary_source_extent(change.symbol).location;
        if (!location.file.empty())
            output << "  Location: " << location.file.string() << ':' << location.line << ':'
                   << location.column << '\n';
        if (change.current.classification.has_value()) {
            output << "  Current: " << to_string(*change.current.classification) << " ("
                   << change.current.confidence << ')';
            if (change.current.suppressed)
                output << " [suppressed]";
            output << '\n';
        }
        output << '\n';
    }
}

void write_json_differential_report(std::ostream& output, const DifferentialReport& report) {
    output << "{\n  \"diff_schema_version\": " << differential_report_schema_version
           << ",\n  \"baseline\": ";
    write_metadata(output, report.baseline);
    output << ",\n  \"current\": ";
    write_metadata(output, report.current);
    output << ",\n  \"summary\": {\"new_symbols\": " << report.new_symbols
           << ", \"newly_unreachable\": " << report.newly_unreachable
           << ", \"removed\": " << report.removed
           << ", \"became_reachable\": " << report.became_reachable
           << ", \"policy_matches\": " << report.policy_matches << "},\n  \"changes\": [";
    for (std::size_t index = 0; index < report.changes.size(); ++index) {
        const auto& change = report.changes[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"kind\": \"" << to_string(change.kind)
               << "\", \"policy_match\": " << (change.policy_match ? "true" : "false")
               << ", \"symbol\": {\"id\": \"" << json::escape(change.symbol.key)
               << "\", \"qualified_name\": \"" << json::escape(change.symbol.qualified_name)
               << "\", \"signature\": \"" << json::escape(change.symbol.signature)
               << "\", \"kind\": \"" << to_string(change.symbol.kind) << "\", \"location\": ";
        write_source_point(output, primary_source_extent(change.symbol).location);
        output << "}, \"baseline\": ";
        write_state(output, change.baseline);
        output << ", \"current\": ";
        write_state(output, change.current);
        output << '}';
    }
    if (!report.changes.empty())
        output << '\n';
    output << "  ]\n}\n";
}

void write_sarif_differential_report(std::ostream& output, const DifferentialReport& report,
                                     const std::filesystem::path& project_root,
                                     std::string_view tool_version) {
    std::set<std::string> rules;
    for (const auto& change : report.changes) {
        if (change.policy_match)
            rules.insert(sarif_rule(change));
    }
    output << "{\n  \"$schema\": \"https://json.schemastore.org/sarif-2.1.0.json\",\n"
           << "  \"version\": \"2.1.0\",\n  \"runs\": [{\n"
           << "    \"tool\": {\"driver\": {\"name\": \"cxx-dead\", \"version\": \""
           << json::escape(tool_version)
           << "\", \"informationUri\": "
              "\"https://github.com/gobha-me/cxx-dead\", \"rules\": [";
    std::size_t rule_index = 0;
    for (const auto& rule : rules) {
        if (rule_index++ != 0)
            output << ", ";
        output << "{\"id\": \"" << json::escape(rule)
               << "\", \"shortDescription\": {\"text\": \"New dead-code debt\"}}";
    }
    output << "]}},\n    \"results\": [";
    std::size_t result_index = 0;
    for (const auto& change : report.changes) {
        if (!change.policy_match)
            continue;
        if (result_index++ != 0)
            output << ",";
        const auto classification = to_string(*change.current.classification);
        output << "\n      {\"ruleId\": \"" << json::escape(sarif_rule(change))
               << "\", \"level\": \"warning\", \"message\": {\"text\": \""
               << json::escape(display_name(change.symbol) + " is " + std::string(classification) +
                               " (" + std::string(to_string(change.kind)) + ")")
               << "\"}, \"partialFingerprints\": {\"cxxDeadSymbolId/v1\": \""
               << json::escape(change.symbol.key) << "\"}, \"properties\": {\"change\": \""
               << to_string(change.kind) << "\", \"classification\": \"" << classification
               << "\", \"confidence\": " << change.current.confidence << ", \"target\": ";
        write_nullable_string(output, report.current.target_name);
        output << '}';
        const auto& extent = primary_source_extent(change.symbol);
        const auto uri = sarif_uri(extent.location.file, project_root);
        if (!uri.empty() && extent.location.line != 0U) {
            output << ", \"locations\": [{\"physicalLocation\": {\"artifactLocation\": {\"uri\": \""
                   << json::escape(uri)
                   << "\"}, \"region\": {\"startLine\": " << extent.location.line;
            if (extent.location.column != 0U)
                output << ", \"startColumn\": " << extent.location.column;
            if (extent.end.line != 0U)
                output << ", \"endLine\": " << extent.end.line;
            if (extent.end.column != 0U)
                output << ", \"endColumn\": " << extent.end.column + extent.end.token_length;
            output << "}}}]";
        }
        output << '}';
    }
    if (result_index != 0U)
        output << '\n';
    output << "    ]\n  }]\n}\n";
}

} // namespace cxx_dead
