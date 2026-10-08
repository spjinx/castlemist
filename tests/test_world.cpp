/// @file
/// @brief Tests for the world layer (WorldScene types).

#include "test_framework.h"

#include "castlemist/world/world_scene.h"

CM_TEST(world, default_scene_is_empty) {
    castlemist::world::WorldScene scene;
    CHECK_FALSE(scene.terrain.present);
    CHECK(scene.props.empty());
    CHECK(scene.models.empty());
    CHECK(scene.collision.instances.empty());
    CHECK(scene.warnings.empty());
    CHECK_FALSE(scene.hasBounds);
}
