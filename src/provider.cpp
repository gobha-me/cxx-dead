#include "cxx_dead/provider.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace cxx_dead {

namespace {

using KeySet = std::set<std::string, std::less<>>;

std::string context_at(const std::filesystem::path& path, std::string_view context) {
    return path.string() + ": " + std::string(context);
}

void require_mapping(const YAML::Node& node, const KeySet& allowed,
                     const std::filesystem::path& path, std::string_view context) {
    if (!node.IsMap())
        throw std::runtime_error(context_at(path, context) + " must be a mapping");
    KeySet seen;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar())
            throw std::runtime_error(context_at(path, context) + " contains a non-scalar key");
        const auto key = entry.first.as<std::string>();
        if (!allowed.contains(key))
            throw std::runtime_error(context_at(path, context) + " contains unknown key '" + key +
                                     "'");
        if (!seen.insert(key).second)
            throw std::runtime_error(context_at(path, context) + " contains duplicate key '" + key +
                                     "'");
    }
}

const YAML::Node require_node(const YAML::Node& mapping, std::string_view key,
                              const std::filesystem::path& path, std::string_view context) {
    const auto node = mapping[std::string(key)];
    if (!node)
        throw std::runtime_error(context_at(path, context) + " requires '" + std::string(key) +
                                 "'");
    return node;
}

std::string require_string(const YAML::Node& mapping, std::string_view key,
                           const std::filesystem::path& path, std::string_view context) {
    const auto node = require_node(mapping, key, path, context);
    if (!node.IsScalar())
        throw std::runtime_error(context_at(path, context) + "." + std::string(key) +
                                 " must be a string");
    const auto value = node.as<std::string>();
    if (value.empty())
        throw std::runtime_error(context_at(path, context) + "." + std::string(key) +
                                 " cannot be empty");
    return value;
}

SymbolSelector parse_selector(const YAML::Node& node, const std::filesystem::path& path,
                              std::string_view context) {
    require_mapping(node, {"id", "linkage_name", "qualified_name", "signature"}, path, context);
    SymbolSelector selector;
    if (node["id"])
        selector.id = require_string(node, "id", path, context);
    if (node["linkage_name"])
        selector.linkage_name = require_string(node, "linkage_name", path, context);
    if (node["qualified_name"])
        selector.qualified_name = require_string(node, "qualified_name", path, context);
    if (node["signature"])
        selector.signature = require_string(node, "signature", path, context);
    const auto identities = static_cast<int>(!selector.id.empty()) +
                            static_cast<int>(!selector.linkage_name.empty()) +
                            static_cast<int>(!selector.qualified_name.empty());
    if (identities != 1)
        throw std::runtime_error(context_at(path, context) +
                                 " must set exactly one of id, linkage_name, or qualified_name");
    if (!selector.signature.empty() && selector.qualified_name.empty())
        throw std::runtime_error(context_at(path, context) + ".signature requires qualified_name");
    return selector;
}

Evidence parse_evidence(const YAML::Node& node, std::string_view provider,
                        const std::filesystem::path& path, std::string_view context) {
    return {.provider = std::string(provider),
            .reason = require_string(node, "reason", path, context)};
}

template <typename Function>
void parse_sequence(const YAML::Node& root, std::string_view key, const std::filesystem::path& path,
                    Function&& parse_item) {
    const auto sequence = root[std::string(key)];
    if (!sequence)
        return;
    if (!sequence.IsSequence())
        throw std::runtime_error(context_at(path, key) + " must be a sequence");
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        parse_item(sequence[index], std::string(key) + "[" + std::to_string(index) + "]");
    }
}

auto selector_key(const SymbolSelector& selector) {
    return std::tuple{selector.id, selector.linkage_name, selector.qualified_name,
                      selector.signature};
}

} // namespace

ProviderPolicy load_provider_config(const std::filesystem::path& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error(path.string() + ": invalid YAML: " + error.what());
    }
    require_mapping(root,
                    {"schema_version", "provider", "roots", "edges", "escapes", "suppressions",
                     "callback_registrations"},
                    path, "document");
    const auto version_node = require_node(root, "schema_version", path, "document");
    int version = 0;
    try {
        version = version_node.as<int>();
    } catch (const YAML::Exception&) {
        throw std::runtime_error(path.string() + ": schema_version must be an integer");
    }
    if (version != provider_config_schema_version) {
        throw std::runtime_error(path.string() + ": unsupported provider schema_version " +
                                 std::to_string(version));
    }

    ProviderPolicy policy;
    policy.source = std::filesystem::absolute(path).lexically_normal();
    policy.provider = require_string(root, "provider", path, "document");

    parse_sequence(root, "roots", path, [&](const YAML::Node& item, const std::string& context) {
        require_mapping(item, {"symbol", "reason"}, path, context);
        policy.roots.push_back({
            .symbol = parse_selector(require_node(item, "symbol", path, context), path,
                                     context + ".symbol"),
            .evidence = parse_evidence(item, policy.provider, path, context),
        });
    });
    parse_sequence(root, "edges", path, [&](const YAML::Node& item, const std::string& context) {
        require_mapping(item, {"from", "to", "reason"}, path, context);
        policy.edges.push_back({
            .from =
                parse_selector(require_node(item, "from", path, context), path, context + ".from"),
            .to = parse_selector(require_node(item, "to", path, context), path, context + ".to"),
            .evidence = parse_evidence(item, policy.provider, path, context),
        });
    });
    parse_sequence(root, "escapes", path, [&](const YAML::Node& item, const std::string& context) {
        require_mapping(item, {"symbol", "from", "reason"}, path, context);
        ProviderEscapeFact fact{
            .symbol = parse_selector(require_node(item, "symbol", path, context), path,
                                     context + ".symbol"),
            .from = std::nullopt,
            .evidence = parse_evidence(item, policy.provider, path, context),
        };
        if (item["from"])
            fact.from = parse_selector(item["from"], path, context + ".from");
        policy.escapes.push_back(std::move(fact));
    });
    parse_sequence(root, "suppressions", path,
                   [&](const YAML::Node& item, const std::string& context) {
                       require_mapping(item, {"symbol", "reason"}, path, context);
                       policy.suppressions.push_back({
                           .symbol = parse_selector(require_node(item, "symbol", path, context),
                                                    path, context + ".symbol"),
                           .evidence = parse_evidence(item, policy.provider, path, context),
                       });
                   });
    parse_sequence(root, "callback_registrations", path,
                   [&](const YAML::Node& item, const std::string& context) {
                       require_mapping(item, {"callee", "argument_index", "reason"}, path, context);
                       const auto index_node = require_node(item, "argument_index", path, context);
                       std::size_t argument_index = 0;
                       try {
                           argument_index = index_node.as<std::size_t>();
                       } catch (const YAML::Exception&) {
                           throw std::runtime_error(
                               context_at(path, context) +
                               ".argument_index must be a non-negative integer");
                       }
                       policy.callback_registrations.push_back({
                           .callee = parse_selector(require_node(item, "callee", path, context),
                                                    path, context + ".callee"),
                           .argument_index = argument_index,
                           .evidence = parse_evidence(item, policy.provider, path, context),
                           .require_unique = true,
                       });
                   });
    return policy;
}

bool matches(const Symbol& symbol, const SymbolSelector& selector) {
    if (!selector.id.empty() && symbol.key != selector.id)
        return false;
    if (!selector.linkage_name.empty() && symbol.identity.linkage_name != selector.linkage_name)
        return false;
    if (!selector.qualified_name.empty() && symbol.qualified_name != selector.qualified_name)
        return false;
    return selector.signature.empty() || symbol.signature == selector.signature;
}

std::string describe(const SymbolSelector& selector) {
    if (!selector.id.empty())
        return "id '" + selector.id + "'";
    if (!selector.linkage_name.empty())
        return "linkage_name '" + selector.linkage_name + "'";
    auto result = "qualified_name '" + selector.qualified_name + "'";
    if (!selector.signature.empty())
        result += " with signature '" + selector.signature + "'";
    return result;
}

SymbolId resolve_unique(const Graph& graph, const SymbolSelector& selector,
                        std::string_view context) {
    std::vector<SymbolId> matches_found;
    for (SymbolId id = 0; id < graph.symbols().size(); ++id) {
        if (matches(graph.symbols()[id], selector))
            matches_found.push_back(id);
    }
    if (matches_found.size() != 1U) {
        throw std::runtime_error(std::string(context) + " selector " + describe(selector) +
                                 " matched " + std::to_string(matches_found.size()) +
                                 " symbols; expected exactly one");
    }
    return matches_found.front();
}

void canonicalize_callback_registration_rules(std::vector<CallbackRegistrationRule>& rules) {
    std::ranges::sort(rules, [](const auto& left, const auto& right) {
        return std::tuple{selector_key(left.callee), left.argument_index, left.evidence.provider,
                          left.evidence.reason, left.require_unique} <
               std::tuple{selector_key(right.callee), right.argument_index, right.evidence.provider,
                          right.evidence.reason, right.require_unique};
    });
    rules.erase(std::ranges::unique(rules).begin(), rules.end());
}

void apply_provider_policies(Graph& graph, const std::vector<ProviderPolicy>& policies) {
    if (policies.empty())
        return;
    for (const auto& policy : policies) {
        const auto source = policy.source.string();
        for (const auto& rule : policy.callback_registrations) {
            if (rule.require_unique)
                static_cast<void>(resolve_unique(graph, rule.callee, source + ": callback"));
        }
        for (const auto& fact : policy.roots) {
            graph.add_root(resolve_unique(graph, fact.symbol, source + ": root"),
                           RootKind::Provider, fact.evidence);
        }
        for (const auto& fact : policy.edges) {
            graph.add_edge(resolve_unique(graph, fact.from, source + ": edge.from"),
                           resolve_unique(graph, fact.to, source + ": edge.to"), EdgeKind::Provider,
                           fact.evidence);
        }
        for (const auto& fact : policy.escapes) {
            const auto from = fact.from.has_value()
                                  ? std::optional<SymbolId>{resolve_unique(
                                        graph, *fact.from, source + ": escape.from")}
                                  : std::nullopt;
            graph.add_escape(resolve_unique(graph, fact.symbol, source + ": escape.symbol"),
                             EscapeKind::Provider, fact.evidence, from);
        }
        for (const auto& fact : policy.suppressions) {
            graph.add_suppression(resolve_unique(graph, fact.symbol, source + ": suppression"),
                                  fact.evidence);
        }
    }
    graph.canonicalize();
}

} // namespace cxx_dead
