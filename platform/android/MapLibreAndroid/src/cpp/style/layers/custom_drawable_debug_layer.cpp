#include "custom_drawable_debug_layer.hpp"

#include <mbgl/style/layers/debug_cube_layer_host.hpp>

#include <string>

namespace mbgl {
namespace android {

DebugCubeLayer::DebugCubeLayer(
    jni::JNIEnv& env, const jni::String& layerId, jni::jdouble lat, jni::jdouble lon, jni::jdouble sizeMeters)
    : Layer(std::make_unique<mbgl::style::CustomDrawableLayer>(
          jni::Make<std::string>(env, layerId),
          std::make_unique<mbgl::style::DebugCubeLayerHost>(mbgl::LatLng{lat, lon}, sizeMeters))) {}

DebugCubeLayer::DebugCubeLayer(mbgl::style::CustomDrawableLayer& coreLayer)
    : Layer(coreLayer) {}

DebugCubeLayer::DebugCubeLayer(std::unique_ptr<mbgl::style::CustomDrawableLayer> coreLayer)
    : Layer(std::move(coreLayer)) {}

DebugCubeLayer::~DebugCubeLayer() = default;

namespace {
jni::Local<jni::Object<Layer>> createJavaPeer(jni::JNIEnv& env, Layer* layer) {
    static auto& javaClass = jni::Class<DebugCubeLayer>::Singleton(env);
    static auto constructor = javaClass.GetConstructor<jni::jlong>(env);
    return javaClass.New(env, constructor, reinterpret_cast<jni::jlong>(layer));
}
} // namespace

DebugCubeJavaLayerPeerFactory::~DebugCubeJavaLayerPeerFactory() = default;

jni::Local<jni::Object<Layer>> DebugCubeJavaLayerPeerFactory::createJavaLayerPeer(jni::JNIEnv& env,
                                                                                  mbgl::style::Layer& layer) {
    return createJavaPeer(env, new DebugCubeLayer(static_cast<mbgl::style::CustomDrawableLayer&>(layer)));
}

jni::Local<jni::Object<Layer>> DebugCubeJavaLayerPeerFactory::createJavaLayerPeer(
    jni::JNIEnv& env, std::unique_ptr<mbgl::style::Layer> layer) {
    return createJavaPeer(env,
                          new DebugCubeLayer(std::unique_ptr<mbgl::style::CustomDrawableLayer>(
                              static_cast<mbgl::style::CustomDrawableLayer*>(layer.release()))));
}

void DebugCubeJavaLayerPeerFactory::registerNative(jni::JNIEnv& env) {
    // Lookup the class
    static auto& javaClass = jni::Class<DebugCubeLayer>::Singleton(env);

    // Register the peer
    jni::RegisterNativePeer<DebugCubeLayer>(
        env,
        javaClass,
        "nativePtr",
        jni::MakePeer<DebugCubeLayer, const jni::String&, jni::jdouble, jni::jdouble, jni::jdouble>,
        "initialize",
        "finalize");
}

} // namespace android
} // namespace mbgl
