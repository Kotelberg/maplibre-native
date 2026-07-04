#include <mbgl/gfx/context.hpp>

#include <mbgl/gfx/offscreen_texture.hpp>

namespace mbgl {
namespace gfx {

std::unique_ptr<OffscreenTexture> Context::createOffscreenTexture(Size size,
                                                                  TextureChannelDataType type,
                                                                  bool /*depth*/,
                                                                  bool /*stencil*/) {
    // Default fallback for backends without a depth-capable offscreen target: a color-only target.
    // Backends that support a depth attachment (Metal here) override this.
    return createOffscreenTexture(size, type);
}

} // namespace gfx
} // namespace mbgl
