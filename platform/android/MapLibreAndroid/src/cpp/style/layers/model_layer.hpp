// This file is generated. Edit scripts/generate-style-code.js, then run `make style-code`.

#pragma once

#include "layer.hpp"
#include "../transition_options.hpp"
#include <mbgl/layermanager/model_layer_factory.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <jni/jni.hpp>

namespace mbgl {
namespace android {

class ModelLayer : public Layer {
public:
    using SuperTag = Layer;
    static constexpr auto Name() { return "org/maplibre/android/style/layers/ModelLayer"; };

    ModelLayer(jni::JNIEnv&, jni::String&, jni::String&);

    ModelLayer(mbgl::style::ModelLayer&);

    ModelLayer(std::unique_ptr<mbgl::style::ModelLayer>);

    ~ModelLayer();

    // Properties

    jni::Local<jni::Object<jni::ObjectTag>> getModelId(jni::JNIEnv&);

    jni::Local<jni::Object<jni::ObjectTag>> getModelOpacity(jni::JNIEnv&);
    void setModelOpacityTransition(jni::JNIEnv&, jlong duration, jlong delay);
    jni::Local<jni::Object<TransitionOptions>> getModelOpacityTransition(jni::JNIEnv&);

    jni::Local<jni::Object<jni::ObjectTag>> getModelScale(jni::JNIEnv&);
    void setModelScaleTransition(jni::JNIEnv&, jlong duration, jlong delay);
    jni::Local<jni::Object<TransitionOptions>> getModelScaleTransition(jni::JNIEnv&);

    jni::Local<jni::Object<jni::ObjectTag>> getModelRotation(jni::JNIEnv&);
    void setModelRotationTransition(jni::JNIEnv&, jlong duration, jlong delay);
    jni::Local<jni::Object<TransitionOptions>> getModelRotationTransition(jni::JNIEnv&);

    jni::Local<jni::Object<jni::ObjectTag>> getModelFootprint(jni::JNIEnv&);
    void setModelFootprintTransition(jni::JNIEnv&, jlong duration, jlong delay);
    jni::Local<jni::Object<TransitionOptions>> getModelFootprintTransition(jni::JNIEnv&);

    // Model asset registry (runtime SDK API; not part of the style spec). A
    // flat, alternating [id0, path0, id1, path1, ...] array mirrors the darwin
    // NSDictionary binding without requiring a JNI Map marshaller.
    jni::Local<jni::Array<jni::String>> getModelAssets(jni::JNIEnv&);
    void setModelAssets(jni::JNIEnv&, const jni::Array<jni::String>&, const jni::Array<jni::String>&);

}; // class ModelLayer

class ModelJavaLayerPeerFactory final : public JavaLayerPeerFactory, public mbgl::ModelLayerFactory {
public:
    ~ModelJavaLayerPeerFactory() override;

    // JavaLayerPeerFactory overrides.
    jni::Local<jni::Object<Layer>> createJavaLayerPeer(jni::JNIEnv&, mbgl::style::Layer&) final;
    jni::Local<jni::Object<Layer>> createJavaLayerPeer(jni::JNIEnv& env, std::unique_ptr<mbgl::style::Layer>) final;

    void registerNative(jni::JNIEnv&) final;

    LayerFactory* getLayerFactory() final { return this; }

}; // class ModelJavaLayerPeerFactory

} // namespace android
} // namespace mbgl
