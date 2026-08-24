#include "cxx_dead/json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <sstream>

namespace cxx_dead::json {

ParseError::ParseError(std::size_t offset, std::string message)
    : std::runtime_error("JSON parse error at byte " + std::to_string(offset) + ": " + message),
      offset_(offset) {}

Value::Value(std::nullptr_t) : storage_(nullptr) {}
Value::Value(bool value) : storage_(value) {}
Value::Value(double value) : storage_(value) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

bool Value::is_null() const {
    return std::holds_alternative<std::nullptr_t>(storage_);
}
bool Value::is_bool() const {
    return std::holds_alternative<bool>(storage_);
}
bool Value::is_number() const {
    return std::holds_alternative<double>(storage_);
}
bool Value::is_string() const {
    return std::holds_alternative<std::string>(storage_);
}
bool Value::is_array() const {
    return std::holds_alternative<Array>(storage_);
}
bool Value::is_object() const {
    return std::holds_alternative<Object>(storage_);
}

bool Value::as_bool() const {
    return std::get<bool>(storage_);
}
double Value::as_number() const {
    return std::get<double>(storage_);
}
const std::string& Value::as_string() const {
    return std::get<std::string>(storage_);
}
const Value::Array& Value::as_array() const {
    return std::get<Array>(storage_);
}
const Value::Object& Value::as_object() const {
    return std::get<Object>(storage_);
}

const Value* Value::find(std::string_view key) const {
    if (!is_object()) {
        return nullptr;
    }
    const auto iterator = as_object().find(key);
    return iterator == as_object().end() ? nullptr : &iterator->second;
}

std::string Value::string_or(std::string_view key, std::string fallback) const {
    const auto* value = find(key);
    return value != nullptr && value->is_string() ? value->as_string() : std::move(fallback);
}

bool Value::bool_or(std::string_view key, bool fallback) const {
    const auto* value = find(key);
    return value != nullptr && value->is_bool() ? value->as_bool() : fallback;
}

namespace {

class Parser {
  public:
    explicit Parser(std::string_view input) : input_(input) {}

    Value parse_document() {
        skip_space();
        auto result = parse_value();
        skip_space();
        if (position_ != input_.size()) {
            fail("unexpected trailing content");
        }
        return result;
    }

  private:
    [[noreturn]] void fail(std::string message) const {
        throw ParseError(position_, std::move(message));
    }

    void skip_space() {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    Value parse_value() {
        skip_space();
        if (position_ >= input_.size()) {
            fail("expected a value");
        }
        switch (input_[position_]) {
        case 'n':
            return parse_literal("null", Value(nullptr));
        case 't':
            return parse_literal("true", Value(true));
        case 'f':
            return parse_literal("false", Value(false));
        case '"':
            return Value(parse_string());
        case '[':
            return parse_array();
        case '{':
            return parse_object();
        default:
            if (input_[position_] == '-' ||
                (input_[position_] >= '0' && input_[position_] <= '9')) {
                return parse_number();
            }
            fail("invalid value");
        }
    }

    Value parse_literal(std::string_view literal, Value value) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
        }
        position_ += literal.size();
        return value;
    }

    static void append_utf8(std::string& output, std::uint32_t code_point) {
        if (code_point <= 0x7fU) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else if (code_point <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
    }

    std::uint32_t parse_hex_quad() {
        if (position_ + 4U > input_.size()) {
            fail("incomplete unicode escape");
        }
        std::uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[position_++];
            result <<= 4U;
            if (c >= '0' && c <= '9')
                result |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                result |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                result |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                fail("invalid unicode escape");
        }
        return result;
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') {
                return result;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                fail("unescaped control character in string");
            }
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (position_ >= input_.size()) {
                fail("incomplete escape sequence");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                auto code_point = parse_hex_quad();
                if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                    if (position_ + 2U > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1U] != 'u') {
                        fail("missing low unicode surrogate");
                    }
                    position_ += 2U;
                    const auto low = parse_hex_quad();
                    if (low < 0xdc00U || low > 0xdfffU) {
                        fail("invalid low unicode surrogate");
                    }
                    code_point = 0x10000U + ((code_point - 0xd800U) << 10U) + (low - 0xdc00U);
                }
                append_utf8(result, code_point);
                break;
            }
            default:
                fail("invalid escape sequence");
            }
        }
        fail("unterminated string");
    }

    Value parse_number() {
        const auto begin = position_;
        if (consume('-') && position_ >= input_.size())
            fail("incomplete number");
        if (consume('0')) {
            // A leading zero is complete unless followed by a fractional/exponent part.
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
                fail("invalid number");
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                fail("invalid fractional part");
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                fail("invalid exponent");
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        const auto token = input_.substr(begin, position_ - begin);
        double number = 0;
        const auto conversion = std::from_chars(token.data(), token.data() + token.size(), number);
        if (conversion.ec != std::errc{} || !std::isfinite(number)) {
            fail("number is not representable");
        }
        return Value(number);
    }

    Value parse_array() {
        expect('[');
        skip_space();
        Value::Array result;
        if (consume(']'))
            return Value(std::move(result));
        while (true) {
            result.push_back(parse_value());
            skip_space();
            if (consume(']'))
                return Value(std::move(result));
            expect(',');
            skip_space();
        }
    }

    Value parse_object() {
        expect('{');
        skip_space();
        Value::Object result;
        if (consume('}'))
            return Value(std::move(result));
        while (true) {
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("expected an object key");
            }
            auto key = parse_string();
            skip_space();
            expect(':');
            skip_space();
            auto [unused, inserted] = result.emplace(std::move(key), parse_value());
            if (!inserted)
                fail("duplicate object key");
            skip_space();
            if (consume('}'))
                return Value(std::move(result));
            expect(',');
            skip_space();
        }
    }

    std::string_view input_;
    std::size_t position_{0};
};

} // namespace

Value parse(std::string_view input) {
    return Parser(input).parse_document();
}

std::string escape(std::string_view input) {
    std::ostringstream output;
    for (const char raw_character : input) {
        const auto c = static_cast<unsigned char>(raw_character);
        switch (c) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (c < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                output << "\\u00" << digits[c >> 4U] << digits[c & 0x0fU];
            } else {
                output << static_cast<char>(c);
            }
        }
    }
    return output.str();
}

} // namespace cxx_dead::json
