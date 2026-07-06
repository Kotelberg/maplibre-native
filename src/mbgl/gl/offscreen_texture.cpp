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
        // A depth-capable target's color texture commonly stores packed non-color data (e.g. an
        // encoded depth value) rather than a color to be filtered. Hardware bilinear filtering
        // would blend those packed bytes into a meaningless value, so force NEAREST sampling
        // whenever a depth attachment is requested (matches mtl::Texture2D's equivalent sampler
        // override). Color-only offscreen targets keep the default Linear filter.
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
                // Attach a depth-only renderbuffer alongside the color texture so a real hardware
                // depth test can run against this offscreen target (e.g. to keep the nearest-camera
                // fragment per texel across multiple draws, rather than last-write-wins). The color
                // texture is still what's sampled afterwards; the depth buffer itself is write-only
                // here and is never read back.
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
