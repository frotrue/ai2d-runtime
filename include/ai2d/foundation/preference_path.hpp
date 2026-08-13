#pragma once

#include <cstddef>
#include <string_view>

namespace ai2d {

[[nodiscard]] inline bool safe_preference_component(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U) return false;
    const auto ascii_alpha = [](const char character) noexcept {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z');
    };
    const auto ascii_digit = [](const char character) noexcept {
        return character >= '0' && character <= '9';
    };
    if (!ascii_alpha(value.front()) && !ascii_digit(value.front())) return false;
    for (const char character : value) {
        if (!ascii_alpha(character) && !ascii_digit(character) &&
            character != '.' && character != '_' && character != '-') {
            return false;
        }
    }
    if (value.back() == '.') return false;

    const auto first_dot = value.find('.');
    const auto basename = value.substr(0U, first_dot);
    const auto upper = [](const char character) noexcept {
        return character >= 'a' && character <= 'z'
                   ? static_cast<char>(character - ('a' - 'A'))
                   : character;
    };
    const auto equals_ascii = [&](const std::string_view candidate) noexcept {
        if (basename.size() != candidate.size()) return false;
        for (std::size_t index = 0U; index < basename.size(); ++index) {
            if (upper(basename[index]) != candidate[index]) return false;
        }
        return true;
    };
    if (equals_ascii("CON") || equals_ascii("PRN") || equals_ascii("AUX") ||
        equals_ascii("NUL")) {
        return false;
    }
    if (basename.size() == 4U && basename[3U] >= '1' && basename[3U] <= '9') {
        const char first = upper(basename[0U]);
        const char second = upper(basename[1U]);
        const char third = upper(basename[2U]);
        if ((first == 'C' && second == 'O' && third == 'M') ||
            (first == 'L' && second == 'P' && third == 'T')) {
            return false;
        }
    }
    return true;
}

} // namespace ai2d
