#include "ai2d/foundation/json_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("JsonWriter emits locale-independent nested JSON") {
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("text");
    writer.value("line\n\"quoted\"");
    writer.key("values");
    writer.begin_array();
    writer.value(std::int64_t{-7});
    writer.value(std::uint64_t{9U});
    writer.value(1.25);
    writer.value(true);
    writer.null_value();
    writer.end_array();
    writer.end_object();

    REQUIRE(writer.complete());
    REQUIRE(writer.str() == R"({"text":"line\n\"quoted\"","values":[-7,9,1.25,true,null]})");
}

TEST_CASE("JsonWriter preserves UTF-8 bytes while escaping ASCII control characters") {
    ai2d::JsonWriter writer{};
    writer.value("\xED\x95\x9C\n");
    REQUIRE(writer.complete());
    CHECK(writer.str() == "\"\xED\x95\x9C\\n\"");
}

TEST_CASE("JsonWriter maps non-finite numbers to null") {
    ai2d::JsonWriter writer{};
    writer.begin_array();
    writer.value(std::numeric_limits<double>::infinity());
    writer.value(std::numeric_limits<double>::quiet_NaN());
    writer.end_array();
    REQUIRE(writer.complete());
    REQUIRE(writer.str() == "[null,null]");
}

TEST_CASE("JsonWriter detects incomplete object values") {
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("missing");
    writer.end_object();
    REQUIRE_FALSE(writer.valid());
    REQUIRE_FALSE(writer.complete());
}
