#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cxx_dead::json {

class ParseError : public std::runtime_error {
  public:
    ParseError(std::size_t offset, std::string message);

    [[nodiscard]] std::size_t offset() const noexcept {
        return offset_;
    }

  private:
    std::size_t offset_;
};

class Value {
  public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    explicit Value(std::nullptr_t);
    explicit Value(bool value);
    explicit Value(double value);
    explicit Value(std::string value);
    explicit Value(Array value);
    explicit Value(Object value);

    [[nodiscard]] bool is_null() const;
    [[nodiscard]] bool is_bool() const;
    [[nodiscard]] bool is_number() const;
    [[nodiscard]] bool is_string() const;
    [[nodiscard]] bool is_array() const;
    [[nodiscard]] bool is_object() const;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;

    [[nodiscard]] const Value* find(std::string_view key) const;
    [[nodiscard]] std::string string_or(std::string_view key, std::string fallback = {}) const;
    [[nodiscard]] bool bool_or(std::string_view key, bool fallback = false) const;

  private:
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Storage storage_{nullptr};
};

[[nodiscard]] Value parse(std::string_view input);
[[nodiscard]] std::string escape(std::string_view input);

} // namespace cxx_dead::json
