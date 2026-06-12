#pragma once

#include <jni/jni.hpp>
#include <mbgl/layermanager/model_layer_factory.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include "layer.hpp"

namespace mbgl {
namespace android {

// JNI peer for the experimental `model` style layer (M6). The constructor
// registers GLB assets (parallel id/path arrays) and wires data-driven
// model-rotation/model-scale/model-footprint from feature properties
// bearing/size/footprint. A null modelID renders each feature's `model-id`
// property; a non-null one pins a single asset.
class ModelLayerAndroid : public Layer {
public:
    using SuperTag = Layer;
    static constexpr auto Name() { return "org/maplibre/android/style/layers/ModelLayer"; };

    static void registerNative(jni::JNIEnv&);

    ModelLayerAndroid(jni::JNIEnv&,
                      const jni::String&,
                      const jni::String&,
                      const jni::Array<jni::String>&,
                      const jni::Array<jni::String>&,
                      const jni::String&);
    ModelLayerAndroid(mbgl::style::ModelLayer&);
    ModelLayerAndroid(std::unique_ptr<mbgl::style::ModelLayer>);
    ~ModelLayerAndroid();

    jni::Local<jni::Object<Layer>> createJavaPeer(jni::JNIEnv&);
};

class ModelJavaLayerPeerFactory final : public JavaLayerPeerFactory, public mbgl::ModelLayerFactory {
public:
    ~ModelJavaLayerPeerFactory() override;

    jni::Local<jni::Object<Layer>> createJavaLayerPeer(jni::JNIEnv&, mbgl::style::Layer&) final;
    jni::Local<jni::Object<Layer>> createJavaLayerPeer(jni::JNIEnv&, std::unique_ptr<mbgl::style::Layer>) final;

    void registerNative(jni::JNIEnv&) final;

    LayerFactory* getLayerFactory() final { return this; }
};

} // namespace android
} // namespace mbgl
