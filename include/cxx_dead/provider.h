#pragma once

#include "cxx_dead/graph.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cxx_dead {

inline constexpr int provider_config_schema_version = 1;

struct SymbolSelector {
    std::string id;
    std::string linkage_name;
    std::string qualified_name;
    std::string signature;

    SymbolSelector() = default;
    SymbolSelector(std::string_view qualified) : qualified_name(qualified) {}

    bool operator==(const SymbolSelector&) const = default;
};

struct CallbackRegistrationRule {
    SymbolSelector callee;
    std::size_t argument_index{0};
    Evidence evidence{.provider = "callback_registration", .reason = {}};
    bool require_unique{false};

    bool operator==(const CallbackRegistrationRule&) const = default;
};

struct ProviderRootFact {
    SymbolSelector symbol;
    Evidence evidence;
};

struct ProviderEdgeFact {
    SymbolSelector from;
    SymbolSelector to;
    Evidence evidence;
};

struct ProviderEscapeFact {
    SymbolSelector symbol;
    std::optional<SymbolSelector> from;
    Evidence evidence;
};

struct ProviderSuppressionFact {
    SymbolSelector symbol;
    Evidence evidence;
};

struct ProviderPolicy {
    std::filesystem::path source;
    std::string provider;
    std::vector<ProviderRootFact> roots;
    std::vector<ProviderEdgeFact> edges;
    std::vector<ProviderEscapeFact> escapes;
    std::vector<ProviderSuppressionFact> suppressions;
    std::vector<CallbackRegistrationRule> callback_registrations;
};

[[nodiscard]] ProviderPolicy load_provider_config(const std::filesystem::path& path);
[[nodiscard]] bool matches(const Symbol& symbol, const SymbolSelector& selector);
[[nodiscard]] std::string describe(const SymbolSelector& selector);
[[nodiscard]] SymbolId resolve_unique(const Graph& graph, const SymbolSelector& selector,
                                      std::string_view context);
void canonicalize_callback_registration_rules(std::vector<CallbackRegistrationRule>& rules);
void apply_provider_policies(Graph& graph, const std::vector<ProviderPolicy>& policies);

} // namespace cxx_dead
