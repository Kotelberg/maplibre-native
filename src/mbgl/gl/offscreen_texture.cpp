#include <mbgl/gl/offscreen_texture.hpp>
#include <mbgl/gl/renderable_resource.hpp>
#include <mbgl/gl/context.hpp>
#include <mbgl/gl/framebuffer.hpp>

namespace mbgl {
namespace gl {

class OffscreenTextureResource final : public gl::RenderableResource {
public:
    OffscreenTextureResource(gl::Context& context_,
                             const Size size_,
                             const gfx::TextureChannelDataType type_,
                             const bool depth_)
        : context(context_),
          size(size_),
          type(type_),
          depth(depth_) {
        assert(!size.isEmpty());
        texture = context.createTexture2D();
        texture->setSize(size);
        texture->setFormat(gfx::TexturePixelType::RGBA, type);
        // Depth-capable target = the shadow map, whose RGBA8 texels are PACKED light-space depth.
        // It MUST be sampled NEAREST: hardware bilinear would blend the packed bytes into garbage
        // depth values, producing noisy/blocky shadows. (Metal forces nearest via a shader sampler;
        // on GL the filter comes from the texture's sampler config, so set it here.) The receiver
        // does its own depth-compare-then-blend PCF. Other offscreen targets keep Linear.
        texture->setSamplerConfiguration({.filter = depth_ ? gfx::TextureFilterType::Nearest
                                                           : gfx::TextureFilterType::Linear,
                                          .wrapU = gfx::TextureWrapType::Clamp,
                                          .wrapV = gfx::TextureWrapType::Clamp});
    }

    ~OffscreenTextureResource() noexcept override = default;

    void bind() override {
        if (!framebuffer) {
            assert(texture);
            texture->create();
            if (depth) {
                // Shadow caster target: attach a 32F depth-only renderbuffer so the hardware depth
                // test keeps the NEAREST-to-light caster's packed-depth color (reliable inter-
                // building occlusion). The color texture (RGBA8 packed depth) is what receivers
                // sample; the depth buffer itself is not sampled. 32F (not 24-bit) avoids roof-edge
                // self-shadow acne where a roof and its sun-facing wall share a shadow-map texel.
                depthBuffer = context.createRenderbuffer<gfx::RenderbufferPixelType::Depth>(size);
                framebuffer = context.createFramebuffer(*texture, *depthBuffer);
            } else {
                framebuffer = context.createFramebuffer(*texture);
            }
        } else {
            context.bindFramebuffer = framebuffer->framebuffer;
        }

        context.activeTextureUnit = 0;
        context.scissorTest = {0, 0, 0, 0};
        context.viewport = {.x = 0, .y = 0, .size = size};
    }

    PremultipliedImage readStillImage() {
        assert(framebuffer);
        context.bindFramebuffer = framebuffer->framebuffer;
        return context.readFramebuffer<PremultipliedImage>(size);
    }

    gfx::Texture2DPtr& getTexture() {
        assert(texture);
        return texture;
    }

private:
    gl::Context& context;
    const Size size;
    gfx::Texture2DPtr texture;
    const gfx::TextureChannelDataType type;
    const bool depth;
    std::optional<gfx::Renderbuffer<gfx::RenderbufferPixelType::Depth>> depthBuffer;
    std::optional<gl::Framebuffer> framebuffer;
};

OffscreenTexture::OffscreenTexture(gl::Context& context,
                                   const Size size_,
                                   const gfx::TextureChannelDataType type,
                                   const bool depth)
    : gfx::OffscreenTexture(size, std::make_unique<OffscreenTextureResource>(context, size_, type, depth)) {}

bool OffscreenTexture::isRenderable() {
    try {
        getResource<OffscreenTextureResource>().bind();
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

PremultipliedImage OffscreenTexture::readStillImage() {
    return getResource<OffscreenTextureResource>().readStillImage();
}

const gfx::Texture2DPtr& OffscreenTexture::getTexture() {
    return getResource<OffscreenTextureResource>().getTexture();
}

} // namespace gl
} // namespace mbgl
