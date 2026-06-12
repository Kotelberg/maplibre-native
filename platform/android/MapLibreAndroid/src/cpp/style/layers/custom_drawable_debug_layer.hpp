#pragma once

#include <jni/jni.hpp>
#include <mbgl/style/layers/custom_drawable_layer.hpp>
#include <mbgl/layermanager/custom_drawable_layer_factory.hpp>
#include "layer.hpp"

namespace mbgl {
namespace android {

// M1 debug cube layer peer: renders a fixed-size cube at a coordinate to
// validate placement and depth ahead of the model layer. Development only.
class DebugCubeLayer : public Layer {
public:
    using SuperTag = Layer;
    static constexpr auto Name() { return "org/maplibre/android/style/layers/DebugCubeLayer"; };

    static void registerNative(jni::JNIEnv&);

    DebugCubeLayer(jni::JNIEnv&, const jni::String&, jni::jdouble, jni::jdouble, jni::jdouble);
    DebugCubeLayer(mbgl::style::CustomDrawableLayer&);
    DebugCubeLayer(std::unique_ptr<mbgl::style::CustomDrawableLayer>);
    ~DebugCubeLayer();

    jni::Local<jni::Object<Layer>> createJavaPeer(jni::JNIEnv&);
}; // class DebugCubeLayer

class DebugCubeJavaLayerPeerFactory final : public JavaLayerPeerFactory, public mbgl::CustomDrawableLayerFactory {
public:
    ~DebugCubeJavaLayerPeerFactory() override;

    // JavaLayerPeerFactory overrides.
    jni::Local<jni::Object<Layer>> createJavaLayerPeer(jni::JNIEnv&, mbgl::style::Layer&) final;
    jni::Local<jni::Object<Layer>> createJavaLayerPeer(jni::JNIEnv& env, std::unique_ptr<mbgl::style::Layer>) final;

    void registerNative(jni::JNIEnv&) final;

    LayerFactory* getLayerFactory() final { return this; }

}; // class DebugCubeJavaLayerPeerFactory

} // namespace android
} // namespace mbgl
