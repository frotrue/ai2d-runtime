#include "ai2d/foundation/json_writer.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <system_error>

namespace ai2d {

bool JsonWriter::before_value() {
    if (!valid_) {
        return false;
    }
    if (stack_.empty()) {
        if (root_written_) {
            valid_ = false;
            return false;
        }
        root_written_ = true;
        return true;
    }

    auto& context = stack_.back();
    if (context.kind == Kind::object) {
        if (!context.expects_value) {
            valid_ = false;
            return false;
        }
        context.expects_value = false;
        return true;
    }

    if (!context.first) {
        output_.push_back(',');
    }
    context.first = false;
    return true;
}

void JsonWriter::append_escaped(const std::string_view text) {
    constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    output_.push_back('"');
    for (const char value : text) {
        const auto character = static_cast<unsigned char>(value);
        switch (character) {
        case '"': output_ += "\\\""; break;
        case '\\': output_ += "\\\\"; break;
        case '\b': output_ += "\\b"; break;
        case '\f': output_ += "\\f"; break;
        case '\n': output_ += "\\n"; break;
        case '\r': output_ += "\\r"; break;
        case '\t': output_ += "\\t"; break;
        default:
            if (character < 0x20U) {
                output_ += "\\u00";
                output_.push_back(hex[(character >> 4U) & 0x0FU]);
                output_.push_back(hex[character & 0x0FU]);
            } else {
                output_.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output_.push_back('"');
}

void JsonWriter::begin_object() {
    if (before_value()) {
        output_.push_back('{');
        stack_.push_back({Kind::object, true, false});
    }
}

void JsonWriter::end_object() {
    if (stack_.empty() || stack_.back().kind != Kind::object || stack_.back().expects_value) {
        valid_ = false;
        return;
    }
    stack_.pop_back();
    output_.push_back('}');
}

void JsonWriter::begin_array() {
    if (before_value()) {
        output_.push_back('[');
        stack_.push_back({Kind::array, true, false});
    }
}

void JsonWriter::end_array() {
    if (stack_.empty() || stack_.back().kind != Kind::array) {
        valid_ = false;
        return;
    }
    stack_.pop_back();
    output_.push_back(']');
}

void JsonWriter::key(const std::string_view name) {
    if (!valid_ || stack_.empty() || stack_.back().kind != Kind::object || stack_.back().expects_value) {
        valid_ = false;
        return;
    }
    auto& context = stack_.back();
    if (!context.first) {
        output_.push_back(',');
    }
    context.first = false;
    append_escaped(name);
    output_.push_back(':');
    context.expects_value = true;
}

void JsonWriter::value(const std::string_view text) {
    if (before_value()) {
        append_escaped(text);
    }
}

void JsonWriter::value(const char* const text) {
    if (text == nullptr) {
        null_value();
        return;
    }
    value(std::string_view{text});
}

void JsonWriter::value(const std::int64_t number) {
    if (!before_value()) {
        return;
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    if (result.ec != std::errc{}) {
        valid_ = false;
        return;
    }
    output_.append(buffer.data(), result.ptr);
}

void JsonWriter::value(const std::uint64_t number) {
    if (!before_value()) {
        return;
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    if (result.ec != std::errc{}) {
        valid_ = false;
        return;
    }
    output_.append(buffer.data(), result.ptr);
}

void JsonWriter::value(const double number) {
    if (!std::isfinite(number)) {
        null_value();
        return;
    }
    if (!before_value()) {
        return;
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number, std::chars_format::general);
    if (result.ec != std::errc{}) {
        valid_ = false;
        return;
    }
    output_.append(buffer.data(), result.ptr);
}

void JsonWriter::value(const bool boolean) {
    if (before_value()) {
        output_ += boolean ? "true" : "false";
    }
}

void JsonWriter::null_value() {
    if (before_value()) {
        output_ += "null";
    }
}

bool JsonWriter::complete() const noexcept {
    return valid_ && root_written_ && stack_.empty();
}

std::string JsonWriter::take() {
    if (!complete()) {
        return {};
    }
    return std::move(output_);
}

} // namespace ai2d
