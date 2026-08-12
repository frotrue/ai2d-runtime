#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ai2d {

class JsonWriter final {
public:
    JsonWriter() = default;

    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    void key(std::string_view name);
    void value(std::string_view text);
    void value(const char* text);
    void value(std::int64_t number);
    void value(std::uint64_t number);
    void value(double number);
    void value(bool boolean);
    void null_value();

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& str() const noexcept { return output_; }
    [[nodiscard]] std::string take();

private:
    enum class Kind : std::uint8_t { object, array };
    struct Context final {
        Kind kind{};
        bool first{true};
        bool expects_value{false};
    };

    bool before_value();
    void append_escaped(std::string_view text);

    std::string output_{};
    std::vector<Context> stack_{};
    bool root_written_{false};
    bool valid_{true};
};

} // namespace ai2d
