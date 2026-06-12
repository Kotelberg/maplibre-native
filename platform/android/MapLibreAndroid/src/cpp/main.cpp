#include "jni.hpp"
#include "jni_native.hpp"
#include <jni/jni.hpp>

#if MLN_WITH_FILAMENT_MODELS
// Statically-linked Filament needs its VM registration (normally done by
// libfilament-jni.so's own JNI_OnLoad); without it PlatformEGLAndroid panics
// with "JNI_OnLoad() has not been called".
namespace filament {
class VirtualMachineEnv {
public:
    static void JNI_OnLoad(JavaVM* vm) noexcept;
};
} // namespace filament
#endif

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    assert(vm != nullptr);
#if MLN_WITH_FILAMENT_MODELS
    filament::VirtualMachineEnv::JNI_OnLoad(vm);
#endif
    mbgl::android::registerNatives(vm);
    return JNI_VERSION_1_6;
}
