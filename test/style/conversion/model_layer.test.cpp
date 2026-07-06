#include <mbgl/test/util.hpp>

#include <mbgl/style/conversion/json.hpp>
#include <mbgl/style/conversion/layer.hpp>
#include <mbgl/style/conversion/stringify.hpp>
#include <mbgl/style/layers/model_layer.hpp>

#include <rapidjson/prettywriter.h>

using namespace mbgl;
using namespace mbgl::style;
using namespace mbgl::style::conversion;

namespace {

std::unique_ptr<Layer> parseLayer(const std::string& src) {
    Error error;
    auto layer = convertJSON<std::unique_ptr<Layer>>(src, error);
    if (layer) return std::move(*layer);
    return nullptr;
}

std::string stringifyLayer(const Value& value) {
    rapidjson::StringBuffer s;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(s);
    writer.SetIndent(' ', 2u);
    stringify(writer, value);
    return s.GetString();
}

} // namespace

// A fully specified model layer parses every spec property to a constant value.
TEST(StyleConversion, ModelLayerConstants) {
    auto layer = parseLayer(R"JSON({
        "type": "model",
        "id": "buildings",
        "source": "points",
        "layout": {
            "model-id": "tower"
        },
        "paint": {
            "model-scale": 42,
            "model-rotation": 90,
            "model-footprint": 2,
            "model-opacity": 0.5
        }
    })JSON");

    ASSERT_NE(nullptr, layer);
    ASSERT_STREQ("model", layer->getTypeInfo()->type);
    auto* model = static_cast<ModelLayer*>(layer.get());

    ASSERT_TRUE(model->getModelId().isConstant());
    EXPECT_EQ("tower", model->getModelId().asConstant());

    ASSERT_TRUE(model->getModelScale().isConstant());
    EXPECT_EQ(42.0f, model->getModelScale().asConstant());

    ASSERT_TRUE(model->getModelRotation().isConstant());
    EXPECT_EQ(90.0f, model->getModelRotation().asConstant());

    ASSERT_TRUE(model->getModelFootprint().isConstant());
    EXPECT_EQ(2.0f, model->getModelFootprint().asConstant());

    ASSERT_TRUE(model->getModelOpacity().isConstant());
    EXPECT_EQ(0.5f, model->getModelOpacity().asConstant());
}

// An omitted property is left undefined on the layer, and the codegen'd static
// defaults carry the values declared in the style spec.
TEST(StyleConversion, ModelLayerDefaults) {
    auto layer = parseLayer(R"JSON({
        "type": "model",
        "id": "m",
        "source": "points"
    })JSON");

    ASSERT_NE(nullptr, layer);
    auto* model = static_cast<ModelLayer*>(layer.get());

    EXPECT_TRUE(model->getModelId().isUndefined());
    EXPECT_TRUE(model->getModelScale().isUndefined());
    EXPECT_TRUE(model->getModelRotation().isUndefined());
    EXPECT_TRUE(model->getModelFootprint().isUndefined());
    EXPECT_TRUE(model->getModelOpacity().isUndefined());

    EXPECT_EQ(20.0f, ModelLayer::getDefaultModelScale().asConstant());
    EXPECT_EQ(0.0f, ModelLayer::getDefaultModelRotation().asConstant());
    EXPECT_EQ(1.0f, ModelLayer::getDefaultModelFootprint().asConstant());
    EXPECT_EQ(1.0f, ModelLayer::getDefaultModelOpacity().asConstant());
    ASSERT_TRUE(ModelLayer::getDefaultModelId().isConstant());
    EXPECT_EQ("", ModelLayer::getDefaultModelId().asConstant());
}

// model-id and the three scale/rotation/footprint paints accept data-driven
// (per-feature) expressions.
TEST(StyleConversion, ModelLayerDataDrivenExpressions) {
    auto layer = parseLayer(R"JSON({
        "type": "model",
        "id": "m",
        "source": "points",
        "layout": {
            "model-id": ["get", "model"]
        },
        "paint": {
            "model-scale": ["get", "height"],
            "model-rotation": ["get", "angle"],
            "model-footprint": ["get", "fp"]
        }
    })JSON");

    ASSERT_NE(nullptr, layer);
    auto* model = static_cast<ModelLayer*>(layer.get());

    EXPECT_TRUE(model->getModelId().isExpression());
    EXPECT_TRUE(model->getModelScale().isExpression());
    EXPECT_TRUE(model->getModelRotation().isExpression());
    EXPECT_TRUE(model->getModelFootprint().isExpression());
}

// model-opacity is data-constant: a per-feature expression must be rejected.
TEST(StyleConversion, ModelLayerOpacityRejectsFeatureExpression) {
    auto layer = parseLayer(R"JSON({
        "type": "model",
        "id": "m",
        "source": "points",
        "paint": {
            "model-opacity": ["get", "o"]
        }
    })JSON");

    EXPECT_EQ(nullptr, layer);
}

// A model layer without a source cannot be created (source is required).
TEST(StyleConversion, ModelLayerRequiresSource) {
    auto layer = parseLayer(R"JSON({
        "type": "model",
        "id": "m"
    })JSON");

    EXPECT_EQ(nullptr, layer);
}

// Unknown properties are rejected by the generated setPropertyInternal.
TEST(StyleConversion, ModelLayerUnknownProperty) {
    ModelLayer layer("m", "points");
    const JSValue value(5.0);
    auto error = layer.setProperty("model-bogus", Convertible(&value));
    EXPECT_TRUE(error.has_value());
}

// The runtime asset registry is a hand-maintained SDK setter: it round-trips
// through the layer, survives cloneRef, and is NOT part of the serialized style.
TEST(Style, ModelLayerAssetRegistry) {
    ModelLayer layer("m", "points");
    EXPECT_TRUE(layer.getModelAssets().empty());

    layer.setModelAssets({{"tower", "/models/tower.glb"}, {"tree", "/models/tree.glb"}});
    ASSERT_EQ(2u, layer.getModelAssets().size());
    EXPECT_EQ("/models/tower.glb", layer.getModelAssets().at("tower"));
    EXPECT_EQ("/models/tree.glb", layer.getModelAssets().at("tree"));

    auto cloned = layer.cloneRef("m2");
    auto* clonedModel = static_cast<ModelLayer*>(cloned.get());
    EXPECT_EQ(2u, clonedModel->getModelAssets().size());
    EXPECT_EQ("/models/tower.glb", clonedModel->getModelAssets().at("tower"));
}

// Spec properties round-trip through serialize()/parse; the runtime asset
// registry deliberately does not appear in the serialized style JSON.
TEST(StyleConversion, ModelLayerSerializeRoundtrip) {
    auto layer = parseLayer(R"JSON({
        "type": "model",
        "id": "buildings",
        "source": "points",
        "layout": {
            "model-id": "tower"
        },
        "paint": {
            "model-scale": 42,
            "model-opacity": 0.5
        }
    })JSON");
    ASSERT_NE(nullptr, layer);

    auto* model = static_cast<ModelLayer*>(layer.get());
    model->setModelAssets({{"tower", "/models/tower.glb"}});

    const std::string serialized = stringifyLayer(layer->serialize());
    // The registry is runtime-only; it must never leak into the style document.
    EXPECT_EQ(std::string::npos, serialized.find("model-assets"));
    EXPECT_EQ(std::string::npos, serialized.find("tower.glb"));

    auto roundTripped = parseLayer(serialized);
    ASSERT_NE(nullptr, roundTripped);
    auto* rt = static_cast<ModelLayer*>(roundTripped.get());

    ASSERT_TRUE(rt->getModelId().isConstant());
    EXPECT_EQ("tower", rt->getModelId().asConstant());
    ASSERT_TRUE(rt->getModelScale().isConstant());
    EXPECT_EQ(42.0f, rt->getModelScale().asConstant());
    ASSERT_TRUE(rt->getModelOpacity().isConstant());
    EXPECT_EQ(0.5f, rt->getModelOpacity().asConstant());

    // Assets are not restored from JSON — they are re-registered at runtime.
    EXPECT_TRUE(rt->getModelAssets().empty());
}
