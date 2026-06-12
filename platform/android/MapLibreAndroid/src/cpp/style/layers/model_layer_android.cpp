#include "model_layer_android.hpp"

#include <mbgl/style/expression/dsl.hpp>
#include <mbgl/style/property_expression.hpp>

#include <string>

namespace mbgl {
namespace android {

namespace {

std::unique_ptr<mbgl::style::ModelLayer> makeDemoLayer(jni::JNIEnv& env,
                                                       const jni::String& layerId,
                                                       const jni::String& sourceId,
                                                       const jni::String& glbPath) {
    namespace dsl = mbgl::style::expression::dsl;
    auto layer = std::make_unique<mbgl::style::ModelLayer>(jni::Make<std::string>(env, layerId),
                                                           jni::Make<std::string>(env, sourceId));
    layer->setModelAssets({{"demo", jni::Make<std::string>(env, glbPath)}});
    layer->setModelId(std::string("demo"));
    layer->setModelRotation(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("bearing"))));
    layer->setModelScale(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("size"))));
    // (missing property → expression error → evaluateFor's default 1.0)
    layer->setModelFootprint(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("footprint"))));
    return layer;
}

} // namespace

ModelLayerAndroid::ModelLayerAndroid(jni::JNIEnv& env,
                                     const jni::String& layerId,
                                     const jni::String& sourceId,
                                     const jni::String& glbPath)
    : Layer(makeDemoLayer(env, layerId, sourceId, glbPath)) {}

ModelLayerAndroid::ModelLayerAndroid(mbgl::style::ModelLayer& coreLayer)
    : Layer(coreLayer) {}

ModelLayerAndroid::ModelLayerAndroid(std::unique_ptr<mbgl::style::ModelLayer> coreLayer)
    : Layer(std::move(coreLayer)) {}

ModelLayerAndroid::~ModelLayerAndroid() = default;

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

    jni::RegisterNativePeer<ModelLayerAndroid>(
        env,
        javaClass,
        "nativePtr",
        jni::MakePeer<ModelLayerAndroid, const jni::String&, const jni::String&, const jni::String&>,
        "initialize",
        "finalize");
}

} // namespace android
} // namespace mbgl
