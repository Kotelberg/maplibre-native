#include <mbgl/gfx/context.hpp>

#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/util/logging.hpp>

#include <cassert>

namespace mbgl {
namespace gfx {

std::unique_ptr<OffscreenTexture> Context::createOffscreenTexture(Size size,
                                                                  TextureChannelDataType type,
                                                                  bool depth,
                                                                  bool /*stencil*/) {
    // Default fallback for backends without a depth-capable offscreen target: a color-only target.
    // Backends that support a depth attachment (Metal, OpenGL) override this with their own 4-arg
    // implementation, so this base body should only ever run with depth == false. If a caller asked
    // for a depth-capable target and execution still reached here, the backend forgot to override —
    // silently downgrading to color-only would surface as a confusing failure much later (e.g. a
    // shadow render pass with no depth attachment), so flag it loudly instead.
    if (depth) {
        assert(!"createOffscreenTexture(depth=true) reached the color-only default; the active "
                "backend is missing its own depth-capable override");
        Log::Warning(Event::General,
                      "gfx::Context::createOffscreenTexture: depth-capable target requested but the "
                      "active backend has no depth override; falling back to a color-only target");
    }
    return createOffscreenTexture(size, type);
}

} // namespace gfx
} // namespace mbgl
