#include <mbgl/vulkan/offscreen_texture.hpp>
#include <mbgl/vulkan/context.hpp>
#include <mbgl/vulkan/renderable_resource.hpp>
#include <mbgl/vulkan/renderer_backend.hpp>
#include <mbgl/vulkan/texture2d.hpp>
#include <mbgl/util/logging.hpp>

#include <algorithm>
#include <limits>

namespace mbgl {
namespace vulkan {

class OffscreenTextureResource final : public RenderableResource {
public:
    OffscreenTextureResource(RendererBackend& backend_,
                             const Size size_,
                             const gfx::TextureChannelDataType type_,
                             const bool depth_)
        : RenderableResource(backend_),
          size(size_),
          type(type_),
          depth(depth_) {
        assert(!size.isEmpty());

        extent.width = size.width;
        extent.height = size.height;

        colorTexture = backend.getContext().createTexture2D();
        auto& texture = static_cast<Texture2D&>(*colorTexture);

        texture.setSize(size);
        texture.setFormat(gfx::TexturePixelType::RGBA, type);
        // A depth-capable target's color texture commonly stores packed non-color data (e.g. an
        // encoded depth value) rather than a color to be filtered. Hardware bilinear filtering
        // would blend those packed bytes into a meaningless value, so force NEAREST sampling
        // whenever a depth attachment is requested (matches the gl and mtl equivalents).
        // Color-only offscreen targets keep the default Linear filter.
        texture.setSamplerConfiguration({depth_ ? gfx::TextureFilterType::Nearest : gfx::TextureFilterType::Linear,
                                         gfx::TextureWrapType::Clamp,
                                         gfx::TextureWrapType::Clamp});
        texture.setUsage(Texture2DUsage::Attachment);

        backend.getContext().renderingStats().numFrameBuffers++;
    }

    ~OffscreenTextureResource() noexcept override {
        framebuffer.reset();
        renderPass.reset();
        colorTexture.reset();

        backend.getContext().renderingStats().numFrameBuffers--;
    }

    void bind() override {
        colorTexture->create();
        createRenderPass();
    }

    const vk::UniqueFramebuffer& getFramebuffer() const override { return framebuffer; };

    PremultipliedImage readStillImage() {
        if (!colorTexture) {
            return {};
        }

        const auto& image = static_cast<Texture2D&>(*colorTexture).readImage();
        return image ? image->clone() : PremultipliedImage();
    }

    gfx::Texture2DPtr& getTexture() {
        assert(colorTexture);
        return colorTexture;
    }

    // Shadow caster target: an optional depth-stencil attachment so the hardware depth test keeps the
    // NEAREST-to-light caster's packed-depth color (reliable inter-building occlusion). Mirrors
    // SurfaceRenderableResource::initDepthStencil. The depth buffer itself is never sampled — the color
    // texture (RGBA8 packed depth) is what receivers read.
    void createDepthAttachment() {
        const auto& physicalDevice = backend.getPhysicalDevice();
        const auto& device = backend.getDevice();
        const auto& dispatcher = backend.getDispatcher();

        const std::vector<vk::Format> formats = {
            vk::Format::eD24UnormS8Uint, vk::Format::eD32SfloatS8Uint, vk::Format::eD16UnormS8Uint};
        const auto formatIt = std::find_if(formats.begin(), formats.end(), [&](const auto& format) {
            return physicalDevice.getFormatProperties(format, dispatcher).optimalTilingFeatures &
                   vk::FormatFeatureFlagBits::eDepthStencilAttachment;
        });
        if (formatIt == formats.end()) {
            mbgl::Log::Error(mbgl::Event::Render, "Offscreen depth/stencil format not available");
            return;
        }
        depthFormat = *formatIt;

        const auto imageUsage = vk::ImageUsageFlags() | vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                vk::ImageUsageFlagBits::eTransientAttachment;
        const auto imageCreateInfo = vk::ImageCreateInfo()
                                         .setImageType(vk::ImageType::e2D)
                                         .setFormat(depthFormat)
                                         .setExtent({extent.width, extent.height, 1})
                                         .setMipLevels(1)
                                         .setArrayLayers(1)
                                         .setSamples(vk::SampleCountFlagBits::e1)
                                         .setTiling(vk::ImageTiling::eOptimal)
                                         .setUsage(imageUsage)
                                         .setSharingMode(vk::SharingMode::eExclusive)
                                         .setInitialLayout(vk::ImageLayout::eUndefined);

        VmaAllocationCreateInfo allocCreateInfo = {};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        uint32_t lazyMemoryIndex = 0;
        if (vmaFindMemoryTypeIndex(
                backend.getAllocator(), std::numeric_limits<uint32_t>::max(), &allocCreateInfo, &lazyMemoryIndex) !=
            VK_SUCCESS) {
            allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        }

        depthAllocation = std::make_unique<ImageAllocation>(backend.getAllocator());
        if (!depthAllocation->create(allocCreateInfo, imageCreateInfo)) {
            mbgl::Log::Error(mbgl::Event::Render, "Offscreen depth texture allocation failed");
            depthAllocation.reset();
            return;
        }
        const auto imageViewCreateInfo =
            vk::ImageViewCreateInfo()
                .setImage(depthAllocation->image)
                .setViewType(vk::ImageViewType::e2D)
                .setFormat(depthFormat)
                .setComponents(vk::ComponentMapping())
                .setSubresourceRange(vk::ImageSubresourceRange(
                    vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1));
        depthAllocation->imageView = device->createImageViewUnique(imageViewCreateInfo, nullptr, dispatcher);
    }

    void createRenderPass() {
        if (renderPass) return;

        assert(colorTexture);
        auto& texture = static_cast<Texture2D&>(*colorTexture);

        const bool hasDepth = depth;
        if (hasDepth && !depthAllocation) {
            createDepthAttachment();
        }
        const bool depthReady = hasDepth && depthAllocation;

        const auto colorAttachment = vk::AttachmentDescription(vk::AttachmentDescriptionFlags())
                                         .setFormat(texture.getVulkanFormat())
                                         .setSamples(vk::SampleCountFlagBits::e1)
                                         .setLoadOp(vk::AttachmentLoadOp::eClear)
                                         .setStoreOp(vk::AttachmentStoreOp::eStore)
                                         .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
                                         .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                                         .setInitialLayout(vk::ImageLayout::eUndefined)
                                         .setFinalLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

        const vk::AttachmentReference colorAttachmentRef(0, vk::ImageLayout::eColorAttachmentOptimal);

        const auto depthAttachment = vk::AttachmentDescription(vk::AttachmentDescriptionFlags())
                                         .setFormat(depthReady ? depthFormat : vk::Format::eD24UnormS8Uint)
                                         .setSamples(vk::SampleCountFlagBits::e1)
                                         .setLoadOp(vk::AttachmentLoadOp::eClear)
                                         .setStoreOp(vk::AttachmentStoreOp::eDontCare)
                                         .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
                                         .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                                         .setInitialLayout(vk::ImageLayout::eUndefined)
                                         .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        const vk::AttachmentReference depthAttachmentRef(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);

        auto subpass = vk::SubpassDescription(vk::SubpassDescriptionFlags())
                           .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
                           .setColorAttachments(colorAttachmentRef);
        if (depthReady) {
            subpass.setPDepthStencilAttachment(&depthAttachmentRef);
        }

        // Incoming (EXTERNAL -> 0): before this pass clears/writes the attachments, prior work must
        // be done with them — including the PREVIOUS pass that SAMPLED the color texture in its
        // fragment shaders (write-after-read needs the execution dependency on eFragmentShader).
        const auto subpassSrcStageMask = vk::PipelineStageFlags() | vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                         vk::PipelineStageFlagBits::eLateFragmentTests |
                                         vk::PipelineStageFlagBits::eFragmentShader;

        const auto subpassDstStageMask = vk::PipelineStageFlags() | vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                         vk::PipelineStageFlagBits::eEarlyFragmentTests;

        const auto subpassSrcAccessMask = vk::AccessFlags() | vk::AccessFlagBits::eDepthStencilAttachmentWrite;

        const auto subpassDstAccessMask = vk::AccessFlags() | vk::AccessFlagBits::eColorAttachmentWrite |
                                          vk::AccessFlagBits::eDepthStencilAttachmentWrite;

        const auto subpassDependencyIn = vk::SubpassDependency()
                                             .setSrcSubpass(VK_SUBPASS_EXTERNAL)
                                             .setDstSubpass(0)
                                             .setSrcStageMask(subpassSrcStageMask)
                                             .setDstStageMask(subpassDstStageMask)
                                             .setSrcAccessMask(subpassSrcAccessMask)
                                             .setDstAccessMask(subpassDstAccessMask);

        // Outgoing (0 -> EXTERNAL): make this pass's color writes VISIBLE to fragment-shader reads
        // in later passes that sample the texture. Without an explicit dependency the implicit one
        // applies with dstAccessMask = 0 — it orders the eShaderReadOnlyOptimal layout transition
        // but establishes NO memory visibility, so a subsequent pass may sample stale/partial data
        // (observed on mobile tiled GPUs as flickering garbage in render-to-texture consumers).
        const auto subpassDependencyOut = vk::SubpassDependency()
                                              .setSrcSubpass(0)
                                              .setDstSubpass(VK_SUBPASS_EXTERNAL)
                                              .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
                                              .setDstStageMask(vk::PipelineStageFlagBits::eFragmentShader)
                                              .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                                              .setDstAccessMask(vk::AccessFlagBits::eShaderRead);

        const std::array<vk::SubpassDependency, 2> subpassDependencies = {subpassDependencyIn, subpassDependencyOut};

        const std::array<vk::AttachmentDescription, 2> attachmentsWithDepth = {colorAttachment, depthAttachment};
        auto renderPassCreateInfo = vk::RenderPassCreateInfo().setSubpasses(subpass).setDependencies(subpassDependencies);
        if (depthReady) {
            renderPassCreateInfo.setAttachments(attachmentsWithDepth);
        } else {
            renderPassCreateInfo.setAttachments(colorAttachment);
        }

        renderPass = backend.getDevice()->createRenderPassUnique(
            renderPassCreateInfo, nullptr, backend.getDispatcher());

        const std::array<vk::ImageView, 2> viewsWithDepth = {texture.getVulkanImageView().get(),
                                                             depthReady ? depthAllocation->imageView.get()
                                                                        : vk::ImageView{}};
        auto framebufferCreateInfo = vk::FramebufferCreateInfo()
                                         .setRenderPass(renderPass.get())
                                         .setWidth(extent.width)
                                         .setHeight(extent.height)
                                         .setLayers(1);
        if (depthReady) {
            framebufferCreateInfo.setAttachments(viewsWithDepth);
        } else {
            framebufferCreateInfo.setAttachments(texture.getVulkanImageView().get());
        }

        framebuffer = backend.getDevice()->createFramebufferUnique(
            framebufferCreateInfo, nullptr, backend.getDispatcher());
    }

private:
    const Size size;
    const gfx::TextureChannelDataType type;
    const bool depth = false;
    vk::Format depthFormat = vk::Format::eD24UnormS8Uint;
    std::unique_ptr<ImageAllocation> depthAllocation;

    gfx::Texture2DPtr colorTexture;
    vk::UniqueFramebuffer framebuffer;
};

OffscreenTexture::OffscreenTexture(Context& context,
                                   const Size size_,
                                   const gfx::TextureChannelDataType type,
                                   bool depth,
                                   [[maybe_unused]] bool stencil)
    : gfx::OffscreenTexture(size,
                            std::make_unique<OffscreenTextureResource>(context.getBackend(), size_, type, depth)) {}

bool OffscreenTexture::isRenderable() {
    return true;
}

PremultipliedImage OffscreenTexture::readStillImage() {
    return getResource<OffscreenTextureResource>().readStillImage(); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
}

const gfx::Texture2DPtr& OffscreenTexture::getTexture() {
    return getResource<OffscreenTextureResource>().getTexture();
}

} // namespace vulkan
} // namespace mbgl
