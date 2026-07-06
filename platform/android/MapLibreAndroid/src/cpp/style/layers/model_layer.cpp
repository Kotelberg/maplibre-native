// This file is generated. Edit scripts/generate-style-code.js, then run `make style-code`.

#include "model_layer.hpp"

#include <string>
#include <map>
#include <utility>

#include "../conversion/property_value.hpp"
#include "../conversion/transition_options.hpp"

#include <mbgl/style/layer_impl.hpp>

namespace mbgl {
namespace android {

inline mbgl::style::ModelLayer& toModelLayer(mbgl::style::Layer& layer) {
    return static_cast<mbgl::style::ModelLayer&>(layer);
}

/**
 * Creates an owning peer object (for layers not attached to the map) from the JVM side
 */
ModelLayer::ModelLayer(jni::JNIEnv& env, jni::String& layerId, jni::String& sourceId)
    : Layer(std::make_unique<mbgl::style::ModelLayer>(jni::Make<std::string>(env, layerId),
                                                      jni::Make<std::string>(env, sourceId))) {}

/**
 * Creates a non-owning peer object (for layers currently attached to the map)
 */
ModelLayer::ModelLayer(mbgl::style::ModelLayer& coreLayer)
    : Layer(coreLayer) {}

/**
 * Creates an owning peer object (for layers not attached to the map)
 */
ModelLayer::ModelLayer(std::unique_ptr<mbgl::style::ModelLayer> coreLayer)
    : Layer(std::move(coreLayer)) {}

ModelLayer::~ModelLayer() = default;

// Property getters

jni::Local<jni::Object<>> ModelLayer::getModelId(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<>>>(env, style::ModelLayer::getDefaultModelId()));
    }
    return std::move(*convert<jni::Local<jni::Object<>>>(env, toModelLayer(*layer).getModelId()));
}

jni::Local<jni::Object<>> ModelLayer::getModelOpacity(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<>>>(env, style::ModelLayer::getDefaultModelOpacity()));
    }
    return std::move(*convert<jni::Local<jni::Object<>>>(env, toModelLayer(*layer).getModelOpacity()));
}

jni::Local<jni::Object<TransitionOptions>> ModelLayer::getModelOpacityTransition(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, mbgl::style::TransitionOptions()));
    }
    mbgl::style::TransitionOptions options = toModelLayer(*layer).getModelOpacityTransition();
    return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, options));
}

void ModelLayer::setModelOpacityTransition(jni::JNIEnv&, jlong duration, jlong delay) {
    auto layer = layerPtr.get();
    if (!layer) {
        return;
    }
    mbgl::style::TransitionOptions options;
    options.duration.emplace(mbgl::Milliseconds(duration));
    options.delay.emplace(mbgl::Milliseconds(delay));
    toModelLayer(*layer).setModelOpacityTransition(options);
}

jni::Local<jni::Object<>> ModelLayer::getModelScale(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<>>>(env, style::ModelLayer::getDefaultModelScale()));
    }
    return std::move(*convert<jni::Local<jni::Object<>>>(env, toModelLayer(*layer).getModelScale()));
}

jni::Local<jni::Object<TransitionOptions>> ModelLayer::getModelScaleTransition(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, mbgl::style::TransitionOptions()));
    }
    mbgl::style::TransitionOptions options = toModelLayer(*layer).getModelScaleTransition();
    return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, options));
}

void ModelLayer::setModelScaleTransition(jni::JNIEnv&, jlong duration, jlong delay) {
    auto layer = layerPtr.get();
    if (!layer) {
        return;
    }
    mbgl::style::TransitionOptions options;
    options.duration.emplace(mbgl::Milliseconds(duration));
    options.delay.emplace(mbgl::Milliseconds(delay));
    toModelLayer(*layer).setModelScaleTransition(options);
}

jni::Local<jni::Object<>> ModelLayer::getModelRotation(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<>>>(env, style::ModelLayer::getDefaultModelRotation()));
    }
    return std::move(*convert<jni::Local<jni::Object<>>>(env, toModelLayer(*layer).getModelRotation()));
}

jni::Local<jni::Object<TransitionOptions>> ModelLayer::getModelRotationTransition(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, mbgl::style::TransitionOptions()));
    }
    mbgl::style::TransitionOptions options = toModelLayer(*layer).getModelRotationTransition();
    return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, options));
}

void ModelLayer::setModelRotationTransition(jni::JNIEnv&, jlong duration, jlong delay) {
    auto layer = layerPtr.get();
    if (!layer) {
        return;
    }
    mbgl::style::TransitionOptions options;
    options.duration.emplace(mbgl::Milliseconds(duration));
    options.delay.emplace(mbgl::Milliseconds(delay));
    toModelLayer(*layer).setModelRotationTransition(options);
}

jni::Local<jni::Object<>> ModelLayer::getModelFootprint(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<>>>(env, style::ModelLayer::getDefaultModelFootprint()));
    }
    return std::move(*convert<jni::Local<jni::Object<>>>(env, toModelLayer(*layer).getModelFootprint()));
}

jni::Local<jni::Object<TransitionOptions>> ModelLayer::getModelFootprintTransition(jni::JNIEnv& env) {
    using namespace mbgl::android::conversion;
    auto layer = layerPtr.get();
    if (!layer) {
        return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, mbgl::style::TransitionOptions()));
    }
    mbgl::style::TransitionOptions options = toModelLayer(*layer).getModelFootprintTransition();
    return std::move(*convert<jni::Local<jni::Object<TransitionOptions>>>(env, options));
}

void ModelLayer::setModelFootprintTransition(jni::JNIEnv&, jlong duration, jlong delay) {
    auto layer = layerPtr.get();
    if (!layer) {
        return;
    }
    mbgl::style::TransitionOptions options;
    options.duration.emplace(mbgl::Milliseconds(duration));
    options.delay.emplace(mbgl::Milliseconds(delay));
    toModelLayer(*layer).setModelFootprintTransition(options);
}

// Model asset registry (runtime SDK API; not part of the style spec). Maps
// a `model-id` value to a local glTF/GLB file path; mirrors Style::addImage
// (the host registers assets at runtime, the style references them only by
// id). Marshalled as a flat, alternating [id0, path0, id1, path1, ...]
// array to avoid a JNI Map wrapper.
jni::Local<jni::Array<jni::String>> ModelLayer::getModelAssets(jni::JNIEnv& env) {
    auto layer = layerPtr.get();
    if (!layer) {
        return jni::Array<jni::String>::New(env, 0);
    }
    const auto& assets = toModelLayer(*layer).getModelAssets();
    auto result = jni::Array<jni::String>::New(env, assets.size() * 2);
    std::size_t i = 0;
    for (const auto& entry : assets) {
        result.Set(env, i++, jni::Make<jni::String>(env, entry.first));
        result.Set(env, i++, jni::Make<jni::String>(env, entry.second));
    }
    return result;
}

void ModelLayer::setModelAssets(jni::JNIEnv& env,
                                const jni::Array<jni::String>& ids,
                                const jni::Array<jni::String>& paths) {
    auto layer = layerPtr.get();
    if (!layer) {
        return;
    }
    std::map<std::string, std::string> assets;
    const std::size_t count = std::min(ids.Length(env), paths.Length(env));
    for (std::size_t i = 0; i < count; ++i) {
        assets.emplace(jni::Make<std::string>(env, ids.Get(env, i)), jni::Make<std::string>(env, paths.Get(env, i)));
    }
    toModelLayer(*layer).setModelAssets(std::move(assets));
}

// ModelJavaLayerPeerFactory

ModelJavaLayerPeerFactory::~ModelJavaLayerPeerFactory() = default;

namespace {
jni::Local<jni::Object<Layer>> createJavaPeer(jni::JNIEnv& env, Layer* layer) {
    static auto& javaClass = jni::Class<ModelLayer>::Singleton(env);
    static auto constructor = javaClass.GetConstructor<jni::jlong>(env);
    return javaClass.New(env, constructor, reinterpret_cast<jni::jlong>(layer));
}
} // namespace

jni::Local<jni::Object<Layer>> ModelJavaLayerPeerFactory::createJavaLayerPeer(jni::JNIEnv& env,
                                                                              mbgl::style::Layer& layer) {
    assert(layer.baseImpl->getTypeInfo() == getTypeInfo());
    return createJavaPeer(env, new ModelLayer(toModelLayer(layer)));
}

jni::Local<jni::Object<Layer>> ModelJavaLayerPeerFactory::createJavaLayerPeer(
    jni::JNIEnv& env, std::unique_ptr<mbgl::style::Layer> layer) {
    assert(layer->baseImpl->getTypeInfo() == getTypeInfo());
    return createJavaPeer(env,
                          new ModelLayer(std::unique_ptr<mbgl::style::ModelLayer>(
                              static_cast<mbgl::style::ModelLayer*>(layer.release()))));
}

void ModelJavaLayerPeerFactory::registerNative(jni::JNIEnv& env) {
    // Lookup the class
    static auto& javaClass = jni::Class<ModelLayer>::Singleton(env);

#define METHOD(MethodPtr, name) jni::MakeNativePeerMethod<decltype(MethodPtr), (MethodPtr)>(name)

    // Register the peer
    jni::RegisterNativePeer<ModelLayer>(
        env,
        javaClass,
        "nativePtr",
        jni::MakePeer<ModelLayer, jni::String&, jni::String&>,
        "initialize",
        "finalize",
        METHOD(&ModelLayer::getModelId, "nativeGetModelId"),
        METHOD(&ModelLayer::getModelOpacityTransition, "nativeGetModelOpacityTransition"),
        METHOD(&ModelLayer::setModelOpacityTransition, "nativeSetModelOpacityTransition"),
        METHOD(&ModelLayer::getModelOpacity, "nativeGetModelOpacity"),
        METHOD(&ModelLayer::getModelScaleTransition, "nativeGetModelScaleTransition"),
        METHOD(&ModelLayer::setModelScaleTransition, "nativeSetModelScaleTransition"),
        METHOD(&ModelLayer::getModelScale, "nativeGetModelScale"),
        METHOD(&ModelLayer::getModelRotationTransition, "nativeGetModelRotationTransition"),
        METHOD(&ModelLayer::setModelRotationTransition, "nativeSetModelRotationTransition"),
        METHOD(&ModelLayer::getModelRotation, "nativeGetModelRotation"),
        METHOD(&ModelLayer::getModelFootprintTransition, "nativeGetModelFootprintTransition"),
        METHOD(&ModelLayer::setModelFootprintTransition, "nativeSetModelFootprintTransition"),
        METHOD(&ModelLayer::getModelFootprint, "nativeGetModelFootprint"),
        METHOD(&ModelLayer::getModelAssets, "nativeGetModelAssets"),
        METHOD(&ModelLayer::setModelAssets, "nativeSetModelAssets"));
}

} // namespace android
} // namespace mbgl
