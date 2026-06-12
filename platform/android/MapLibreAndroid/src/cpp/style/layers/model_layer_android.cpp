#include "model_layer_android.hpp"

#include <mbgl/style/expression/dsl.hpp>
#include <mbgl/style/property_expression.hpp>

#include <string>

namespace mbgl {
namespace android {

namespace {

std::unique_ptr<mbgl::style::ModelLayer> makeLayer(jni::JNIEnv& env,
                                                   const jni::String& layerId,
                                                   const jni::String& sourceId,
                                                   const jni::Array<jni::String>& assetIds,
                                                   const jni::Array<jni::String>& assetPaths,
                                                   const jni::String& modelID) {
    namespace dsl = mbgl::style::expression::dsl;
    auto layer = std::make_unique<mbgl::style::ModelLayer>(jni::Make<std::string>(env, layerId),
                                                           jni::Make<std::string>(env, sourceId));

    std::map<std::string, std::string> assets;
    const std::size_t count = std::min(assetIds.Length(env), assetPaths.Length(env));
    for (std::size_t i = 0; i < count; ++i) {
        assets.emplace(jni::Make<std::string>(env, assetIds.Get(env, i)),
                       jni::Make<std::string>(env, assetPaths.Get(env, i)));
    }
    layer->setModelAssets(std::move(assets));

    if (modelID) {
        // Pin a single asset for all features.
        layer->setModelId(jni::Make<std::string>(env, modelID));
    } else {
        // Per-feature: each feature's `model-id` property selects the asset.
        layer->setModelId(
            mbgl::style::PropertyExpression<std::string>(dsl::toString(dsl::get("model-id"))));
    }

    layer->setModelRotation(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("bearing"))));
    layer->setModelScale(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("size"))));
    // (missing property → expression error → evaluateFor's default 1.0)
    layer->setModelFootprint(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("footprint"))));
    // Models appear only at the 3D viewing zooms (matches HataHub's auto-pitch
    // + fill-extrusion threshold).
    layer->setMinZoom(15.0f);
    return layer;
}

} // namespace

ModelLayerAndroid::ModelLayerAndroid(jni::JNIEnv& env,
                                     const jni::String& layerId,
                                     const jni::String& sourceId,
                                     const jni::Array<jni::String>& assetIds,
                                     const jni::Array<jni::String>& assetPaths,
                                     const jni::String& modelID)
    : Layer(makeLayer(env, layerId, sourceId, assetIds, assetPaths, modelID)) {}

ModelLayerAndroid::ModelLayerAndroid(mbgl::style::ModelLayer& coreLayer)
    : Layer(coreLayer) {}

ModelLayerAndroid::ModelLayerAndroid(std::unique_ptr<mbgl::style::ModelLayer> coreLayer)
    : Layer(std::move(coreLayer)) {}

ModelLayerAndroid::~ModelLayerAndroid() = default;

void ModelLayerAndroid::setModelAssets(jni::JNIEnv& env,
                                       const jni::Array<jni::String>& assetIds,
                                       const jni::Array<jni::String>& assetPaths) {
    std::map<std::string, std::string> assets;
    const std::size_t count = std::min(assetIds.Length(env), assetPaths.Length(env));
    for (std::size_t i = 0; i < count; ++i) {
        assets.emplace(jni::Make<std::string>(env, assetIds.Get(env, i)),
                       jni::Make<std::string>(env, assetPaths.Get(env, i)));
    }
    static_cast<mbgl::style::ModelLayer&>(get()).setModelAssets(std::move(assets));
}

namespace {
jni::Local<jni::Object<Layer>> createJavaPeer(jni::JNIEnv& env, Layer* layer) {
    static auto& javaClass = jni::Class<ModelLayerAndroid>::Singleton(env);
    static auto constructor = javaClass.GetConstructor<jni::jlong>(env);
    return javaClass.New(env, constructor, reinterpret_cast<jni::jlong>(layer));
}
} // namespace

ModelJavaLayerPeerFactory::~ModelJavaLayerPeerFactory() = default;

jni::Local<jni::Object<Layer>> ModelJavaLayerPeerFactory::createJavaLayerPeer(jni::JNIEnv& env,
                                                                              mbgl::style::Layer& layer) {
    return createJavaPeer(env, new ModelLayerAndroid(static_cast<mbgl::style::ModelLayer&>(layer)));
}

jni::Local<jni::Object<Layer>> ModelJavaLayerPeerFactory::createJavaLayerPeer(
    jni::JNIEnv& env, std::unique_ptr<mbgl::style::Layer> layer) {
    return createJavaPeer(env,
                          new ModelLayerAndroid(std::unique_ptr<mbgl::style::ModelLayer>(
                              static_cast<mbgl::style::ModelLayer*>(layer.release()))));
}

void ModelJavaLayerPeerFactory::registerNative(jni::JNIEnv& env) {
    static auto& javaClass = jni::Class<ModelLayerAndroid>::Singleton(env);

#define METHOD(MethodPtr, name) jni::MakeNativePeerMethod<decltype(MethodPtr), (MethodPtr)>(name)

    jni::RegisterNativePeer<ModelLayerAndroid>(
        env,
        javaClass,
        "nativePtr",
        jni::MakePeer<ModelLayerAndroid,
                      const jni::String&,
                      const jni::String&,
                      const jni::Array<jni::String>&,
                      const jni::Array<jni::String>&,
                      const jni::String&>,
        "initialize",
        "finalize",
        METHOD(&ModelLayerAndroid::setModelAssets, "nativeSetModelAssets"));
}

} // namespace android
} // namespace mbgl
