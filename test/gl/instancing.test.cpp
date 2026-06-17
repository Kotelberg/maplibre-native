#if MLN_RENDER_BACKEND_OPENGL

#include <mbgl/test/util.hpp>
#include <mbgl/gfx/attribute.hpp>
#include <mbgl/shaders/layer_ubo.hpp> // for MLN_GL_FE_INSTANCING

using namespace mbgl;

TEST(GLInstancing, AttributeBindingCarriesDivisor) {
    gfx::AttributeBinding a{};
    a.instanceDivisor = 1;
    gfx::AttributeBinding b = a;
    EXPECT_EQ(b.instanceDivisor, 1u);
    EXPECT_TRUE(a == b);
    b.instanceDivisor = 0;
    EXPECT_FALSE(a == b); // divisor participates in equality (VAO cache key)
}

#if MLN_GL_FE_INSTANCING

#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/test/stub_geometry_tile_feature.hpp>

namespace {
PropertyMap feInstanceProperties;
} // namespace

// The GL edge-indexed bucket emits one wall instance per outline vertex, kept in lockstep
// (1:1) with the roof verts so the data-driven paint binders index instances correctly.
// Each instance carries this edge's two endpoints + smoothed endpoint normals.
TEST(GLInstancing, BucketEmitsInstancePerOutlineVertex) {
    FillExtrusionBucket::PossiblyEvaluatedLayoutProperties layout;
    FillExtrusionBucket bucket{layout, {}, 5.0f, 1};

    // Closed square ring (z0 disables corner-rounding, so vertices are preserved verbatim).
    GeometryCollection polygon{{{0, 0}, {0, 4096}, {4096, 4096}, {4096, 0}, {0, 0}}};
    bucket.addFeature(StubGeometryTileFeature{{}, FeatureType::Polygon, polygon, feInstanceProperties},
                      polygon,
                      {},
                      PatternLayerMap(),
                      0,
                      CanonicalTileID(0, 0, 0));

    const auto& instances = bucket.glEdgeInstances.vector();
    // 1:1 with the roof verts — the invariant that lets readDataDrivenPaintProperties align.
    ASSERT_EQ(bucket.glEdgeInstances.elements(), bucket.vertices.elements());
    ASSERT_GE(instances.size(), 4u); // at least the four edges of the square

    // First instance describes edge ring[0] -> ring[1] = {0,0} -> {0,4096}.
    EXPECT_EQ(instances[0].a1[0], 0);    // pos0.x
    EXPECT_EQ(instances[0].a1[1], 0);    // pos0.y
    EXPECT_EQ(instances[0].a2[0], 0);    // pos1.x
    EXPECT_EQ(instances[0].a2[1], 4096); // pos1.y
    // Smoothed wall normal at the start endpoint is non-zero (perpendicular computed + packed).
    EXPECT_NE(instances[0].a3[0] | instances[0].a3[1], 0);
}

#endif // MLN_GL_FE_INSTANCING

#endif // MLN_RENDER_BACKEND_OPENGL
