#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

TEST_CASE("Diagnostic codes and JSON fields are stable") {
    auto diagnostic = ai2d::Diagnostic::make(
        ai2d::DiagnosticCode::world_stale_entity,
        ai2d::Severity::error,
        "world",
        "The entity generation is stale");
    diagnostic.context.push_back({"index", std::uint64_t{42U}});
    diagnostic.context.push_back({"alive", false});
    diagnostic.suggestions.push_back("Refresh the entity handle");

    ai2d::JsonWriter writer{};
    ai2d::write_json(writer, diagnostic);

    REQUIRE(writer.complete());
    REQUIRE(writer.str().find("\"code\":\"WORLD_STALE_ENTITY\"") != std::string::npos);
    REQUIRE(writer.str().find("\"severity\":\"error\"") != std::string::npos);
    REQUIRE(writer.str().find("\"index\":42") != std::string::npos);
    REQUIRE(writer.str().find("\"alive\":false") != std::string::npos);
    REQUIRE(writer.str().find("\"source\":{") != std::string::npos);
}
