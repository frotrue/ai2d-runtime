#include "ai2d/renderer2d/render_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("Render queue culls and batches by ascending layer then texture") {
    ai2d::RenderQueue2D queue{};
    queue.reserve(8U);
    const ai2d::TextureHandle texture_a{0U, 1U};
    const ai2d::TextureHandle texture_b{1U, 1U};
    const std::array sprites{
        ai2d::SpriteSubmission2D{{0.0F, 0.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, texture_b, 1, true},
        ai2d::SpriteSubmission2D{{1.0F, 0.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, texture_a, 1, true},
        ai2d::SpriteSubmission2D{{2.0F, 0.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, texture_a, 1, true},
        ai2d::SpriteSubmission2D{{0.0F, 1.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, texture_b, -2, true},
        ai2d::SpriteSubmission2D{{100.0F, 0.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, texture_a, 0, true},
        ai2d::SpriteSubmission2D{{0.0F, 0.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, texture_a, 0, false},
    };
    const auto built = queue.build({{{0.0F, 0.0F}, {10.0F, 10.0F}}, sprites, {}});
    REQUIRE(built);
    CHECK(built->visited == 6U);
    CHECK(built->visible == 4U);
    CHECK(built->culled == 2U);
    CHECK(built->batches == 3U);
    REQUIRE(queue.batches().size() == 3U);
    CHECK(queue.batches()[0].layer == -2);
    CHECK(queue.batches()[1].texture == texture_a);
    CHECK(queue.batches()[1].instance_count == 2U);
    CHECK(queue.batches()[2].texture == texture_b);
}

TEST_CASE("Single layer and texture produces one sprite batch") {
    ai2d::RenderQueue2D queue{};
    queue.reserve(100U);
    std::array<ai2d::SpriteSubmission2D, 100U> sprites{};
    for (std::size_t index = 0U; index < sprites.size(); ++index) {
        sprites[index].position = {static_cast<float>(index % 10U), static_cast<float>(index / 10U)};
        sprites[index].texture = {7U, 2U};
        sprites[index].layer = 3;
    }
    const auto built = queue.build({{{4.5F, 4.5F}, {8.0F, 8.0F}}, sprites, {}});
    REQUIRE(built);
    CHECK(built->visible == 100U);
    CHECK(built->batches == 1U);
    CHECK(queue.batches()[0].instance_count == 100U);
}

TEST_CASE("Render queue rejects growth beyond its explicit capacity") {
    ai2d::RenderQueue2D queue{};
    queue.reserve(1U);
    const std::array sprites{
        ai2d::SpriteSubmission2D{{}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, {0U, 1U}, 0, true},
        ai2d::SpriteSubmission2D{{1.0F, 1.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}, 0.0F, {}, {}, {0U, 1U}, 0, true},
    };
    const auto built = queue.build({{{}, {10.0F, 10.0F}}, sprites, {}});
    REQUIRE_FALSE(built);
    CHECK(built.error().code == ai2d::DiagnosticCode::render_upload_capacity_exceeded);
}

TEST_CASE("Render queue applies pivot consistently to culling and instance data") {
    ai2d::RenderQueue2D queue{};
    queue.reserve(1U);
    const std::array sprites{
        ai2d::SpriteSubmission2D{{11.5F, 0.0F}, {2.0F, 4.0F}, {1.0F, 0.5F}, 0.0F, {}, {}, {0U, 1U}, 0, true},
    };
    const auto built = queue.build({{{0.0F, 0.0F}, {10.0F, 10.0F}}, sprites, {}});
    REQUIRE(built);
    CHECK(built->visible == 1U);
    REQUIRE(queue.instances().size() == 1U);
    CHECK(queue.instances()[0U].clip_size[0] == 0.2F);
    CHECK(queue.instances()[0U].clip_size[1] == 0.4F);
    CHECK(queue.instances()[0U].pivot[0] == 1.0F);
    CHECK(queue.instances()[0U].pivot[1] == 0.5F);
}
