#include <mbgl/test/util.hpp>

#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/conversion/geojson.hpp>
#include <mbgl/style/expression/dsl.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/sources/geojson_source.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/io.hpp>
#include <mbgl/util/run_loop.hpp>

#include <cstdlib>
#include <optional>
#include <string>

using namespace mbgl;
using namespace mbgl::style;

namespace {

// `ModelLayer::setModelAssets` is a runtime-only SDK registry, not a style
// JSON property (see include/mbgl/style/layers/model_layer.hpp -- it
// mirrors `Style::addImage`: the style references a model only by
// `model-id`, and the host registers the id -> local GLB path mapping at
// runtime). Consequently there is no "inject modelAssets via style JSON"
// path for a render-test harness to hook into -- and none is needed: this
// test builds the model layer straight through the same public C++ API a
// real embedder would call, exactly the way test/api/custom_drawable_layer.test.cpp
// and test/api/custom_geometry_source.test.cpp build their layers/sources
// directly in C++ rather than via style JSON. That IS the resolution to the
// "how does a render test register a runtime-only asset" question: the test
// *is* the registration call, not a style-JSON workaround.
// `exerciseFootprint` is scoped to callers that actually reach the
// footprint-consuming baked-mesh path (render_model_layer.cpp only applies
// `model-footprint` to resolved-model instances -- the placeholder-cube
// fallback ignores it entirely), so PlaceholderCube's golden is unaffected
// either way; it's kept an opt-in parameter so that test stays exactly as
// it was before this property was covered.
void addDemoModelLayer(Map& map, const std::optional<std::string>& glbPath, bool exerciseFootprint = false) {
    style::conversion::Error error;
    auto geojson = style::conversion::parseGeoJSON(
        R"({"type":"FeatureCollection","features":[
{"type":"Feature","properties":{"bearing":0,"size":15,"footprint":0.4},"geometry":{"type":"Point","coordinates":[30.5224,50.4505]}},
{"type":"Feature","properties":{"bearing":45,"size":30,"footprint":1.6},"geometry":{"type":"Point","coordinates":[30.5234,50.4495]}},
{"type":"Feature","properties":{"bearing":120,"size":50,"footprint":0.7},"geometry":{"type":"Point","coordinates":[30.5246,50.4503]}}]})",
        error);
    ASSERT_TRUE(static_cast<bool>(geojson)) << error.message;

    auto source = std::make_unique<GeoJSONSource>("model-demo-points");
    source->setGeoJSON(*geojson);
    map.getStyle().addSource(std::move(source));

    namespace dsl = style::expression::dsl;
    auto layer = std::make_unique<ModelLayer>("model-demo", "model-demo-points");
    layer->setModelRotation(PropertyExpression<float>(dsl::number(dsl::get("bearing"))));
    layer->setModelScale(PropertyExpression<float>(dsl::number(dsl::get("size"))));
    if (exerciseFootprint) {
        // model-footprint: x/y scale independent of height (data-driven),
        // mirroring bin/render.cpp's --model-layer harness. Per-feature
        // values (0.4/1.6/0.7) so the property is genuinely covered, not
        // just left at its 1.0 default.
        layer->setModelFootprint(PropertyExpression<float>(dsl::number(dsl::get("footprint"))));
    }
    layer->setModelOpacity(0.9f);
    layer->setMinZoom(15.0f);
    if (glbPath) {
        layer->setModelAssets({{"demo", *glbPath}});
        layer->setModelId(std::string("demo"));
    }
    map.getStyle().addLayer(std::move(layer));
}

} // namespace

TEST(ModelLayer, PlaceholderCube) {
    util::RunLoop loop;
    // Force the render layer's viewport-cover build to happen on the very
    // first frame instead of debouncing across two frames -- see
    // MLN_MODEL_NO_DEBOUNCE in render_model_layer.cpp; without this, a
    // single frontend.render() call could observe stale (empty) drawables.
    setenv("MLN_MODEL_NO_DEBOUNCE", "1", 1);

    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/model_layer/style.json"));
    map.jumpTo(CameraOptions().withCenter(LatLng{50.4501, 30.5234}).withZoom(15.5).withBearing(20.0).withPitch(45.0));

    // No modelAssets registered at all: every feature's `model-id` ("demo")
    // fails to resolve, so all three fall back to the placeholder cube path.
    addDemoModelLayer(map, std::nullopt);

    test::checkImage("test/fixtures/model_layer/placeholder_cube", frontend.render(map).image, 0.0008, 0.1);
}

TEST(ModelLayer, GlbAsset) {
    util::RunLoop loop;
    setenv("MLN_MODEL_NO_DEBOUNCE", "1", 1);

    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/model_layer/style.json"));
    map.jumpTo(CameraOptions().withCenter(LatLng{50.4501, 30.5234}).withZoom(15.5).withBearing(20.0).withPitch(45.0));

    // The CC0 house.glb fixture (see test/fixtures/model_layer/generate_house_glb.py)
    // registered under id "demo" and selected via `model-id` -- exercises
    // the real glTF parse + bake path (glb_mesh_loader.cpp), not just the
    // placeholder. Also exercises `model-footprint` (data-driven, per
    // feature) since this is the only path that actually consumes it.
    addDemoModelLayer(map, std::string("test/fixtures/model_layer/house.glb"), /*exerciseFootprint=*/true);

    test::checkImage("test/fixtures/model_layer/glb_house", frontend.render(map).image, 0.0008, 0.1);
}
