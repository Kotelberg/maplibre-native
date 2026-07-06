#include <mbgl/renderer/model/glb_mesh_loader.hpp>
#include <mbgl/renderer/model/placeholder_mesh.hpp>

#include <mbgl/test/util.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace mbgl;
using namespace mbgl::model;

namespace {

// test/fixtures/model_layer/house.glb (see generate_house_glb.py alongside
// it) is a procedurally-authored, CC0/public-domain fixture: a box (walls,
// textured) + a 4-sided pyramid (roof, flat color) + a small proud quad
// (door, flat color) -- 3 materials total, core glTF 2.0 only (no draco, no
// meshopt, no KHR_mesh_quantization), uint16 indices throughout.
constexpr const char* kHouseGlbPath = "test/fixtures/model_layer/house.glb";

bool nearlyEqual(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

} // namespace

TEST(GlbMeshLoader, LoadsHouseFixture) {
    const BakedModel model = loadGlbMesh(kHouseGlbPath);

    ASSERT_TRUE(model.valid);
    // 3 materials (wall/roof/door); the wall's 12 triangles are further split
    // across per-triangle baked-lambert shade buckets (deterministic given
    // the fixture's fixed geometry + the loader's fixed sun), so there are
    // more than 3 parts overall. Regenerate this fixture/test together if the
    // bake algorithm ever changes the bucketing.
    ASSERT_EQ(model.parts.size(), 5u);

    std::size_t totalVertices = 0;
    std::size_t totalIndices = 0;
    std::size_t texturedParts = 0;
    std::size_t untexturedParts = 0;
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    bool foundRoofColor = false;
    bool foundDoorColor = false;

    for (const auto& part : model.parts) {
        ASSERT_TRUE(part.vertices);
        ASSERT_TRUE(part.indices);

        // Non-empty geometry.
        EXPECT_GT(part.vertices->elements(), 0u);
        EXPECT_GT(part.indices->elements(), 0u);
        // Every part is triangle-soup with no shared vertices in this
        // fixture (the generator never reuses a vertex across faces): one
        // index per emitted vertex, in multiples of 3.
        EXPECT_EQ(part.indices->elements(), part.vertices->elements());
        EXPECT_EQ(part.indices->elements() % 3, 0u);

        // uint16-index-bounds sanity: every index must reference a vertex
        // that actually exists in this part.
        for (std::size_t i = 0; i < part.indices->elements(); ++i) {
            const uint16_t index = part.indices->at(i);
            EXPECT_LT(static_cast<std::size_t>(index), part.vertices->elements());
        }

        totalVertices += part.vertices->elements();
        totalIndices += part.indices->elements();

        if (part.texture) {
            ++texturedParts;
            // Wall material's baseColorFactor is [1,1,1,1] (white) -- the
            // baked-lambert shade scales r/g/b uniformly, so they must stay
            // equal (a colored channel skew here would mean the wrong
            // material got attached to a textured part).
            EXPECT_TRUE(nearlyEqual(part.color.r, part.color.g));
            EXPECT_TRUE(nearlyEqual(part.color.g, part.color.b));
            EXPECT_FLOAT_EQ(part.color.a, 1.0f);
        } else {
            ++untexturedParts;
            EXPECT_FLOAT_EQ(part.color.a, 1.0f);
            // The shade bucket scales r/g/b uniformly, so the ratio between
            // channels is preserved from the material's baseColorFactor --
            // check the ratio (not the absolute value) to assert each
            // material's actual color reached the right part, independent
            // of which bucket/order the loader emitted it in.
            ASSERT_GT(part.color.r, 0.0f);
            const float gOverR = part.color.g / part.color.r;
            const float bOverR = part.color.b / part.color.r;
            // roof: baseColorFactor [0.55, 0.16, 0.14, 1.0]
            if (nearlyEqual(gOverR, 0.16f / 0.55f, 1e-3f) && nearlyEqual(bOverR, 0.14f / 0.55f, 1e-3f)) {
                foundRoofColor = true;
            }
            // door: baseColorFactor [0.30, 0.18, 0.10, 1.0]
            if (nearlyEqual(gOverR, 0.18f / 0.30f, 1e-3f) && nearlyEqual(bOverR, 0.10f / 0.30f, 1e-3f)) {
                foundDoorColor = true;
            }
        }

        for (std::size_t i = 0; i < part.vertices->elements(); ++i) {
            const float z = part.vertices->at(i).position[2];
            minZ = std::min(minZ, z);
            maxZ = std::max(maxZ, z);
        }
    }

    // 3 wall shading buckets (textured), roof + door (flat color, no texture).
    EXPECT_EQ(texturedParts, 3u);
    EXPECT_EQ(untexturedParts, 2u);
    EXPECT_TRUE(foundRoofColor) << "no untextured part matched the roof material's color ratio";
    EXPECT_TRUE(foundDoorColor) << "no untextured part matched the door material's color ratio";

    // wall (12 tri) + roof (4 tri) + door (2 tri) = 18 triangles = 54 verts/indices.
    EXPECT_EQ(totalVertices, 54u);
    EXPECT_EQ(totalIndices, 54u);

    // The loader normalizes so the model's bounding-box height maps to
    // exactly [0, 1] (base-centered, +Z-up) -- the door's bottom edge (at
    // the original mesh's y=0) and the roof's apex (the tallest point) pin
    // the two ends of that range.
    EXPECT_TRUE(nearlyEqual(minZ, 0.0f, 1e-3f));
    EXPECT_TRUE(nearlyEqual(maxZ, 1.0f, 1e-3f));
}

TEST(GlbMeshLoader, MissingFileIsInvalidAndEmpty) {
    // The render layer relies on exactly this contract to decide whether to
    // fall back to the placeholder cube (render_model_layer.cpp checks
    // `baked.valid` after calling loadGlbMesh): a missing/unparseable path
    // must not throw, and must come back invalid with no parts.
    const BakedModel model = loadGlbMesh("test/fixtures/model_layer/does-not-exist.glb");

    EXPECT_FALSE(model.valid);
    EXPECT_TRUE(model.parts.empty());
}

TEST(GlbMeshLoader, PlaceholderCubeShape) {
    CubeVertexVector vertices;
    CubeIndexVector indices;
    buildPlaceholderCube(vertices, indices);

    // 6 faces x 4 vertices (not shared, one UV cell per face).
    EXPECT_EQ(vertices.elements(), 24u);
    // 6 faces x 2 triangles x 3 indices.
    EXPECT_EQ(indices.elements(), 36u);

    for (std::size_t i = 0; i < indices.elements(); ++i) {
        EXPECT_LT(static_cast<std::size_t>(indices.at(i)), vertices.elements());
    }

    // Base-centered on the ground plane, +Z up, height 1 -- same convention
    // BakedModel::Part uses, so the render layer's per-instance scale-by-
    // meters logic is identical for both real and placeholder geometry.
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();
    for (std::size_t i = 0; i < vertices.elements(); ++i) {
        const auto& v = vertices.at(i);
        EXPECT_GE(v.position[0], -0.5f);
        EXPECT_LE(v.position[0], 0.5f);
        EXPECT_GE(v.position[1], -0.5f);
        EXPECT_LE(v.position[1], 0.5f);
        minZ = std::min(minZ, v.position[2]);
        maxZ = std::max(maxZ, v.position[2]);
    }
    EXPECT_TRUE(nearlyEqual(minZ, 0.0f));
    EXPECT_TRUE(nearlyEqual(maxZ, 1.0f));
}
