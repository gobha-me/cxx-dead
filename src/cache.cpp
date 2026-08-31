#include "cxx_dead/cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace cxx_dead {

namespace {

class Sha256 {
  public:
    void update(std::string_view input) {
        for (const char raw_byte : input) {
            const auto byte = static_cast<unsigned char>(raw_byte);
            buffer_[buffer_size_++] = byte;
            if (buffer_size_ == buffer_.size()) {
                transform();
                bit_count_ += 512U;
                buffer_size_ = 0;
            }
        }
    }

    std::string finish() {
        bit_count_ += static_cast<std::uint64_t>(buffer_size_) * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56U) {
            while (buffer_size_ < buffer_.size())
                buffer_[buffer_size_++] = 0;
            transform();
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56U)
            buffer_[buffer_size_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8)
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(bit_count_ >> shift);
        transform();

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto value : state_)
            output << std::setw(8) << value;
        return output.str();
    }

  private:
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };

    void transform() {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(buffer_[offset]) << 24U) |
                           (static_cast<std::uint32_t>(buffer_[offset + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(buffer_[offset + 2U]) << 8U) |
                           static_cast<std::uint32_t>(buffer_[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^
                            (words[index - 15U] >> 3U);
            const auto s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^
                            (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temporary1 = h + sum1 + choose + constants[index] + words[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{0};
    std::uint64_t bit_count_{0};
};

std::string sha256(std::string_view input) {
    Sha256 hash;
    hash.update(input);
    return hash.finish();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot hash cache dependency: " + path.string());
    Sha256 hash;
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())));
    }
    if (!input.eof())
        throw std::runtime_error("cannot read cache dependency: " + path.string());
    return hash.finish();
}

std::string tool_fingerprint(const std::filesystem::path& raw_path) {
    auto path = raw_path;
    if (path == "/proc/self/exe")
        path = std::filesystem::read_symlink(path);
    path = std::filesystem::absolute(path).lexically_normal();
    const auto timestamp = std::filesystem::last_write_time(path).time_since_epoch().count();
    return sha256(path.generic_string() + "\n" + std::to_string(std::filesystem::file_size(path)) +
                  "\n" + std::to_string(timestamp));
}

std::optional<std::filesystem::path> resolve_executable(std::string_view executable) {
    auto path = std::filesystem::path(executable);
    if (path.has_parent_path()) {
        if (path.is_relative())
            path = std::filesystem::absolute(path);
        return std::filesystem::exists(path) ? std::optional{path.lexically_normal()}
                                             : std::nullopt;
    }
    const auto* raw_path = std::getenv("PATH");
    if (raw_path == nullptr)
        return std::nullopt;
    std::string_view search(raw_path);
    while (true) {
        const auto separator = search.find(':');
        auto directory = std::filesystem::path(search.substr(0, separator));
        if (directory.empty())
            directory = std::filesystem::current_path();
        const auto candidate = directory / path;
        if (std::filesystem::exists(candidate))
            return std::filesystem::absolute(candidate).lexically_normal();
        if (separator == std::string_view::npos)
            break;
        search.remove_prefix(separator + 1U);
    }
    return std::nullopt;
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

std::vector<std::string> normalized_arguments(const CompileCommand& command) {
    const std::set<std::string, std::less<>> flags_with_value{
        "-o", "-MF", "-MT", "-MQ", "-MJ", "--serialize-diagnostics", "-dependency-file"};
    const std::set<std::string, std::less<>> removed_flags{
        "-c", "-S", "-E", "-MD", "-MMD", "-MP", "-MG", "-MM", "-M", "-emit-llvm"};
    std::vector<std::string> result;
    const auto compiler_index = compiler_argument_index(command.arguments);
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
            auto possible_file = std::filesystem::path(argument);
            if (possible_file.is_relative())
                possible_file = command.directory / possible_file;
            if (std::filesystem::absolute(possible_file).lexically_normal() == command.file)
                continue;
        }
        result.push_back(argument);
    }
    return result;
}

void append_component(std::ostringstream& output, std::string_view name, std::string_view value) {
    output << name.size() << ':' << name << value.size() << ':' << value;
}

template <typename Range>
void append_paths(std::ostringstream& output, std::string_view name, const Range& paths) {
    append_component(output, name, std::to_string(paths.size()));
    for (const auto& path : paths) {
        if constexpr (requires { path.generic_string(); })
            append_component(output, "path", path.generic_string());
        else
            append_component(output, "value", path);
    }
}

std::filesystem::path entry_path(const std::filesystem::path& directory, std::string_view key) {
    return directory / ("v" + std::to_string(translation_unit_cache_schema_version)) /
           std::string(key.substr(0, 2)) / (std::string(key) + ".cache");
}

void write_point(std::ostream& output, const SourcePoint& point) {
    output << std::quoted(point.file.generic_string()) << ' ' << point.line << ' ' << point.column
           << ' ' << point.offset << ' ' << point.token_length << '\n';
}

SourcePoint read_point(std::istream& input) {
    std::string file;
    SourcePoint point;
    if (!(input >> std::quoted(file) >> point.line >> point.column >> point.offset >>
          point.token_length)) {
        throw std::runtime_error("invalid cached source point");
    }
    point.file = std::move(file);
    return point;
}

void write_extent(std::ostream& output, const SourceExtent& extent) {
    write_point(output, extent.location);
    write_point(output, extent.begin);
    write_point(output, extent.end);
}

SourceExtent read_extent(std::istream& input) {
    return {.location = read_point(input), .begin = read_point(input), .end = read_point(input)};
}

void write_evidence(std::ostream& output, const Evidence& evidence) {
    output << std::quoted(evidence.provider) << ' ' << std::quoted(evidence.reason) << '\n';
}

Evidence read_evidence(std::istream& input) {
    Evidence result;
    if (!(input >> std::quoted(result.provider) >> std::quoted(result.reason)))
        throw std::runtime_error("invalid cached evidence");
    return result;
}

std::size_t checked_count(std::istream& input, std::string_view field) {
    std::size_t result = 0;
    if (!(input >> result) || result > 10'000'000U)
        throw std::runtime_error("invalid cached " + std::string(field) + " count");
    return result;
}

template <typename Enum> Enum checked_enum(int value, int maximum, std::string_view field) {
    if (value < 0 || value > maximum)
        throw std::runtime_error("invalid cached " + std::string(field));
    return static_cast<Enum>(value);
}

void write_entry(std::ostream& output, std::string_view key, TranslationUnitCacheEntry entry) {
    entry.graph.canonicalize();
    output << "cxx-dead-tu-cache " << translation_unit_cache_schema_version << '\n'
           << std::quoted(std::string(key)) << '\n'
           << entry.ast_bytes << ' ' << entry.fact_bytes << '\n';
    output << entry.dependencies.size() << '\n';
    for (const auto& dependency : entry.dependencies)
        output << std::quoted(dependency.path.generic_string()) << ' '
               << std::quoted(dependency.digest) << '\n';
    output << entry.registration_rule_matches.size() << '\n';
    for (const bool matched : entry.registration_rule_matches)
        output << (matched ? 1 : 0) << '\n';
    output << entry.record_hierarchy.size() << '\n';
    for (const auto& record : entry.record_hierarchy) {
        output << std::quoted(record.name) << ' ' << record.bases.size() << '\n';
        for (const auto& base : record.bases)
            output << std::quoted(base) << '\n';
    }
    output << entry.diagnostics.size() << '\n';
    for (const auto& diagnostic : entry.diagnostics)
        output << std::quoted(diagnostic) << '\n';

    output << entry.graph.symbols().size() << '\n';
    for (const auto& symbol : entry.graph.symbols()) {
        output << std::quoted(symbol.key) << '\n'
               << std::quoted(symbol.identity.configuration_id) << '\n'
               << std::quoted(symbol.identity.usr) << '\n'
               << std::quoted(symbol.identity.linkage_name) << '\n'
               << std::quoted(symbol.identity.translation_unit) << '\n'
               << std::quoted(symbol.identity.fallback_anchor) << '\n'
               << static_cast<int>(symbol.identity.quality) << '\n'
               << std::quoted(symbol.name) << '\n'
               << std::quoted(symbol.qualified_name) << '\n'
               << std::quoted(symbol.class_name) << '\n'
               << std::quoted(symbol.signature) << '\n'
               << static_cast<int>(symbol.kind) << ' ' << static_cast<int>(symbol.scope) << ' '
               << symbol.defined << ' ' << symbol.internal_linkage << ' ' << symbol.is_virtual
               << '\n';
        write_extent(output, symbol.source.spelling);
        output << symbol.source.expansion.has_value() << '\n';
        if (symbol.source.expansion.has_value())
            write_extent(output, *symbol.source.expansion);
    }
    output << entry.graph.edges().size() << '\n';
    for (const auto& edge : entry.graph.edges()) {
        output << edge.from << ' ' << edge.to << ' ' << static_cast<int>(edge.kind) << '\n';
        write_evidence(output, edge.evidence);
    }
    output << entry.graph.roots().size() << '\n';
    for (const auto& root : entry.graph.roots()) {
        output << root.symbol << ' ' << static_cast<int>(root.kind) << '\n';
        write_evidence(output, root.evidence);
    }
    output << entry.graph.escapes().size() << '\n';
    for (const auto& escape : entry.graph.escapes()) {
        output << escape.symbol << ' ' << escape.from.has_value() << ' ' << escape.from.value_or(0)
               << ' ' << static_cast<int>(escape.kind) << '\n';
        write_evidence(output, escape.evidence);
    }
    output << entry.graph.suppressions().size() << '\n';
    for (const auto& suppression : entry.graph.suppressions()) {
        output << suppression.symbol << '\n';
        write_evidence(output, suppression.evidence);
    }
}

TranslationUnitCacheEntry read_entry(std::istream& input, std::string_view expected_key) {
    std::string magic;
    int schema = 0;
    std::string key;
    if (!(input >> magic >> schema >> std::quoted(key)) || magic != "cxx-dead-tu-cache" ||
        schema != translation_unit_cache_schema_version || key != expected_key) {
        throw std::runtime_error("cache header, schema, or key mismatch");
    }
    TranslationUnitCacheEntry entry;
    if (!(input >> entry.ast_bytes >> entry.fact_bytes))
        throw std::runtime_error("invalid cached byte counters");
    const auto dependency_count = checked_count(input, "dependency");
    entry.dependencies.reserve(dependency_count);
    for (std::size_t index = 0; index < dependency_count; ++index) {
        std::string path;
        CacheDependency dependency;
        if (!(input >> std::quoted(path) >> std::quoted(dependency.digest)))
            throw std::runtime_error("invalid cached dependency");
        dependency.path = std::move(path);
        entry.dependencies.push_back(std::move(dependency));
    }
    const auto match_count = checked_count(input, "callback match");
    entry.registration_rule_matches.reserve(match_count);
    for (std::size_t index = 0; index < match_count; ++index) {
        int matched = 0;
        if (!(input >> matched) || (matched != 0 && matched != 1))
            throw std::runtime_error("invalid cached callback match");
        entry.registration_rule_matches.push_back(matched != 0);
    }
    const auto record_count = checked_count(input, "record hierarchy");
    entry.record_hierarchy.reserve(record_count);
    for (std::size_t index = 0; index < record_count; ++index) {
        CachedRecordHierarchy record;
        std::size_t base_count = 0;
        if (!(input >> std::quoted(record.name) >> base_count) || base_count > 1'000'000U)
            throw std::runtime_error("invalid cached record hierarchy");
        record.bases.reserve(base_count);
        for (std::size_t base = 0; base < base_count; ++base) {
            std::string name;
            if (!(input >> std::quoted(name)))
                throw std::runtime_error("invalid cached base record");
            record.bases.push_back(std::move(name));
        }
        entry.record_hierarchy.push_back(std::move(record));
    }
    const auto diagnostic_count = checked_count(input, "diagnostic");
    entry.diagnostics.reserve(diagnostic_count);
    for (std::size_t index = 0; index < diagnostic_count; ++index) {
        std::string diagnostic;
        if (!(input >> std::quoted(diagnostic)))
            throw std::runtime_error("invalid cached diagnostic");
        entry.diagnostics.push_back(std::move(diagnostic));
    }

    const auto symbol_count = checked_count(input, "symbol");
    for (std::size_t index = 0; index < symbol_count; ++index) {
        Symbol symbol;
        int quality = 0;
        int kind = 0;
        int scope = 0;
        if (!(input >> std::quoted(symbol.key) >> std::quoted(symbol.identity.configuration_id) >>
              std::quoted(symbol.identity.usr) >> std::quoted(symbol.identity.linkage_name) >>
              std::quoted(symbol.identity.translation_unit) >>
              std::quoted(symbol.identity.fallback_anchor) >> quality >> std::quoted(symbol.name) >>
              std::quoted(symbol.qualified_name) >> std::quoted(symbol.class_name) >>
              std::quoted(symbol.signature) >> kind >> scope >> symbol.defined >>
              symbol.internal_linkage >> symbol.is_virtual)) {
            throw std::runtime_error("invalid cached symbol");
        }
        symbol.identity.quality = checked_enum<IdentityQuality>(quality, 1, "identity quality");
        symbol.kind = checked_enum<SymbolKind>(kind, 4, "symbol kind");
        symbol.scope = checked_enum<SymbolScope>(scope, 3, "symbol scope");
        symbol.source.spelling = read_extent(input);
        bool has_expansion = false;
        if (!(input >> has_expansion))
            throw std::runtime_error("invalid cached expansion marker");
        if (has_expansion)
            symbol.source.expansion = read_extent(input);
        entry.graph.add_or_merge_symbol(std::move(symbol));
    }
    const auto edge_count = checked_count(input, "edge");
    for (std::size_t index = 0; index < edge_count; ++index) {
        SymbolId from = 0;
        SymbolId to = 0;
        int kind = 0;
        if (!(input >> from >> to >> kind))
            throw std::runtime_error("invalid cached edge");
        entry.graph.add_edge(from, to, checked_enum<EdgeKind>(kind, 4, "edge kind"),
                             read_evidence(input));
    }
    const auto root_count = checked_count(input, "root");
    for (std::size_t index = 0; index < root_count; ++index) {
        SymbolId symbol = 0;
        int kind = 0;
        if (!(input >> symbol >> kind))
            throw std::runtime_error("invalid cached root");
        entry.graph.add_root(symbol, checked_enum<RootKind>(kind, 5, "root kind"),
                             read_evidence(input));
    }
    const auto escape_count = checked_count(input, "escape");
    for (std::size_t index = 0; index < escape_count; ++index) {
        SymbolId symbol = 0;
        bool has_from = false;
        SymbolId from = 0;
        int kind = 0;
        if (!(input >> symbol >> has_from >> from >> kind))
            throw std::runtime_error("invalid cached escape");
        entry.graph.add_escape(symbol, checked_enum<EscapeKind>(kind, 2, "escape kind"),
                               read_evidence(input), has_from ? std::optional{from} : std::nullopt);
    }
    const auto suppression_count = checked_count(input, "suppression");
    for (std::size_t index = 0; index < suppression_count; ++index) {
        SymbolId symbol = 0;
        if (!(input >> symbol))
            throw std::runtime_error("invalid cached suppression");
        entry.graph.add_suppression(symbol, read_evidence(input));
    }
    input >> std::ws;
    if (!input.eof())
        throw std::runtime_error("unexpected trailing cache data");
    return entry;
}

} // namespace

StagedCacheWrite::StagedCacheWrite(std::filesystem::path temporary,
                                   std::filesystem::path destination, std::size_t bytes)
    : temporary_(std::move(temporary)), destination_(std::move(destination)), bytes_(bytes) {}

StagedCacheWrite::StagedCacheWrite(StagedCacheWrite&& other) noexcept
    : temporary_(std::move(other.temporary_)), destination_(std::move(other.destination_)),
      bytes_(other.bytes_) {
    other.bytes_ = 0;
}

StagedCacheWrite& StagedCacheWrite::operator=(StagedCacheWrite&& other) noexcept {
    if (this != &other) {
        discard();
        temporary_ = std::move(other.temporary_);
        destination_ = std::move(other.destination_);
        bytes_ = other.bytes_;
        other.bytes_ = 0;
    }
    return *this;
}

StagedCacheWrite::~StagedCacheWrite() {
    discard();
}

void StagedCacheWrite::discard() noexcept {
    if (temporary_.empty())
        return;
    std::error_code error;
    std::filesystem::remove(temporary_, error);
    temporary_.clear();
}

void StagedCacheWrite::commit(std::vector<std::string>& warnings) {
    if (temporary_.empty())
        return;
    std::error_code error;
    std::filesystem::rename(temporary_, destination_, error);
    if (error) {
        std::filesystem::remove(destination_, error);
        error.clear();
        std::filesystem::rename(temporary_, destination_, error);
    }
    if (error) {
        warnings.push_back("could not publish translation-unit cache entry " +
                           destination_.string() + ": " + error.message());
        discard();
        return;
    }
    temporary_.clear();
}

std::string translation_unit_cache_key(const CompileCommand& command, const IndexOptions& options,
                                       IndexFrontend frontend) {
    std::ostringstream components;
    append_component(components, "cache_schema",
                     std::to_string(translation_unit_cache_schema_version));
    append_component(components, "extractor", "cxx-dead-0.16");
    try {
        append_component(components, "cxx_dead_binary", tool_fingerprint("/proc/self/exe"));
    } catch (const std::exception&) {
        append_component(components, "cxx_dead_binary", "unavailable");
    }
    append_component(components, "frontend", std::string(to_string(frontend)));
    append_component(components, "directory", command.directory.generic_string());
    append_component(components, "file", command.file.generic_string());
    append_component(components, "configuration", options.configuration_id);
    append_component(components, "project_root", options.project_root.generic_string());
    append_component(components, "clang", options.clang_executable);
    if (frontend == IndexFrontend::AstJson) {
        if (const auto compiler = resolve_executable(options.clang_executable)) {
            append_component(components, "clang_path", compiler->generic_string());
            try {
                append_component(components, "clang_digest", tool_fingerprint(*compiler));
            } catch (const std::exception&) {
                append_component(components, "clang_digest", "unavailable");
            }
        }
    }
    append_component(components, "ast_filter", options.ast_filter);
    append_component(components, "verbose", options.verbose ? "1" : "0");
    append_component(components, "infer_exports", options.infer_shared_library_exports ? "1" : "0");
    append_component(components, "require_api", options.require_library_api_policy ? "1" : "0");
    append_paths(components, "arguments", normalized_arguments(command));
    for (const auto& argument : command.arguments) {
        if (!argument.starts_with('@') || argument.size() == 1U)
            continue;
        auto response = std::filesystem::path(argument.substr(1));
        if (response.is_relative())
            response = command.directory / response;
        response = std::filesystem::absolute(response).lexically_normal();
        append_component(components, "response_path", response.generic_string());
        try {
            append_component(components, "response_digest", sha256_file(response));
        } catch (const std::exception&) {
            append_component(components, "response_digest", "unavailable");
        }
    }
    append_paths(components, "report_paths", options.report_paths);
    append_paths(components, "excluded_paths", options.excluded_paths);
    append_paths(components, "selected_sources", options.selected_target_sources);
    append_paths(components, "public_headers", options.public_headers);
    for (const auto& rule : options.callback_registration_rules) {
        append_component(components, "callback", describe(rule.callee));
        append_component(components, "argument", std::to_string(rule.argument_index));
        append_component(components, "provider", rule.evidence.provider);
        append_component(components, "reason", rule.evidence.reason);
    }
    for (const char* name :
         {"PATH", "CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
          "GCC_EXEC_PREFIX", "COMPILER_PATH", "LIBRARY_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET",
          "SOURCE_DATE_EPOCH", "CLANG_CONFIG_FILE_SYSTEM_DIR", "CLANG_CONFIG_FILE_USER_DIR", "LANG",
          "LC_ALL"}) {
        if (const auto* value = std::getenv(name); value != nullptr)
            append_component(components, name, value);
    }
    return sha256(components.str());
}

CacheLookupResult load_translation_unit_cache(const std::filesystem::path& directory,
                                              std::string_view key,
                                              const std::filesystem::path& required_source) {
    CacheLookupResult result;
    const auto path = entry_path(directory, key);
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error)
        return result;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open cache entry");
        std::ostringstream contents;
        contents << input.rdbuf();
        if (input.bad())
            throw std::runtime_error("cannot read cache entry");
        const auto serialized = contents.str();
        result.bytes_read = serialized.size();
        std::istringstream parsed(serialized);
        auto entry = read_entry(parsed, key);
        const auto normalized_source =
            std::filesystem::absolute(required_source).lexically_normal();
        if (entry.dependencies.empty() ||
            std::ranges::none_of(entry.dependencies, [&](const CacheDependency& dependency) {
                return dependency.path == normalized_source;
            })) {
            throw std::runtime_error("cache dependency manifest omitted the primary source");
        }
        for (const auto& dependency : entry.dependencies) {
            if (dependency.digest.size() != 64U ||
                !std::ranges::all_of(dependency.digest, [](const char character) {
                    return (character >= '0' && character <= '9') ||
                           (character >= 'a' && character <= 'f');
                })) {
                throw std::runtime_error("invalid cache dependency digest");
            }
            if (sha256_file(dependency.path) != dependency.digest)
                return result;
        }
        result.entry = std::move(entry);
    } catch (const std::exception& exception) {
        result.warnings.push_back("ignored unusable translation-unit cache entry " + path.string() +
                                  ": " + exception.what());
    }
    return result;
}

std::optional<StagedCacheWrite> stage_translation_unit_cache(const std::filesystem::path& directory,
                                                             std::string_view key,
                                                             const TranslationUnitCacheEntry& entry,
                                                             std::vector<std::string>& warnings) {
    std::filesystem::path temporary;
    try {
        const auto destination = entry_path(directory, key);
        std::filesystem::create_directories(destination.parent_path());
        static std::atomic<std::uint64_t> stage_counter{0};
        temporary = destination.string() + ".tmp." + std::to_string(::getpid()) + "." +
                    std::to_string(stage_counter.fetch_add(1));
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot create staged cache entry");
        write_entry(output, key, entry);
        output.flush();
        if (!output)
            throw std::runtime_error("cannot write staged cache entry");
        const auto bytes = static_cast<std::size_t>(output.tellp());
        output.close();
        return StagedCacheWrite(temporary, destination, bytes);
    } catch (const std::exception& exception) {
        if (!temporary.empty()) {
            std::error_code error;
            std::filesystem::remove(temporary, error);
        }
        warnings.push_back("could not stage translation-unit cache entry: " +
                           std::string(exception.what()));
        return std::nullopt;
    }
}

std::vector<CacheDependency> hash_cache_dependencies(std::vector<std::filesystem::path> paths) {
    for (auto& path : paths)
        path = std::filesystem::absolute(path).lexically_normal();
    std::ranges::sort(paths);
    paths.erase(std::ranges::unique(paths).begin(), paths.end());
    std::vector<CacheDependency> result;
    result.reserve(paths.size());
    for (const auto& path : paths)
        result.push_back({.path = path, .digest = sha256_file(path)});
    return result;
}

std::vector<std::filesystem::path>
parse_make_dependencies(const std::filesystem::path& depfile,
                        const std::filesystem::path& working_directory) {
    std::ifstream input(depfile, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read compiler dependency file: " + depfile.string());
    std::string text(std::istreambuf_iterator<char>(input), {});
    std::string flattened;
    flattened.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\\' && index + 1U < text.size() && text[index + 1U] == '\n') {
            flattened.push_back(' ');
            ++index;
        } else {
            flattened.push_back(text[index]);
        }
    }
    bool escaped = false;
    bool after_target = false;
    std::string token;
    std::vector<std::filesystem::path> result;
    const auto flush = [&] {
        if (!after_target || token.empty()) {
            token.clear();
            return;
        }
        auto path = std::filesystem::path(token);
        if (path.is_relative())
            path = working_directory / path;
        result.push_back(std::filesystem::absolute(path).lexically_normal());
        token.clear();
    };
    for (const char character : flattened) {
        if (escaped) {
            token.push_back(character);
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (!after_target && character == ':') {
            token.clear();
            after_target = true;
        } else if (character == ' ' || character == '\t' || character == '\r' ||
                   character == '\n') {
            flush();
        } else {
            token.push_back(character);
        }
    }
    if (escaped)
        token.push_back('\\');
    flush();
    if (!after_target || result.empty())
        throw std::runtime_error("compiler dependency file contained no dependencies");
    return result;
}

std::filesystem::path cache_temporary_path(std::string_view suffix) {
    static std::atomic<std::uint64_t> counter{0};
    return std::filesystem::temp_directory_path() /
           ("cxx-dead-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)) +
            std::string(suffix));
}

} // namespace cxx_dead
