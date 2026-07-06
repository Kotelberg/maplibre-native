#import "MLNFoundation_Private.h"
#import "MLNLoggingConfiguration_Private.h"
#import "MLNMapView+Metal.h"
#import "MLNRendererConfiguration.h"

#import <mbgl/mtl/renderable_resource.hpp>

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>

#import <Metal/Metal.hpp>

@interface MLNMapViewImplDelegate : NSObject <MTKViewDelegate>
@end

@implementation MLNMapViewImplDelegate {
  MLNMapViewMetalImpl* _impl;
}

- (instancetype)initWithImpl:(MLNMapViewMetalImpl*)impl {
  if (self = [super init]) {
    _impl = impl;
  }
  return self;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView*)view {
  _impl->render();
}

@end

namespace {
/// Returns the MSAA sample count to use for the map surface `MTKView`, given the value
/// requested via `MLNRendererConfiguration` and what `device` actually supports. `requested`
/// is expected to already be one of the sample counts `MLNRendererConfiguration` allows
/// (1, 2, 4, or 8); this further reduces it to the highest value the device supports, so
/// that a request of `1` (the default) never touches `MTKView.sampleCount` at all.
NSUInteger MLNSupportedMetalSampleCount(id<MTLDevice> device, NSUInteger requested) {
  NSUInteger sampleCount = requested;
  while (sampleCount > 1 && ![device supportsTextureSampleCount:sampleCount]) {
    sampleCount /= 2;
  }
  return sampleCount;
}
} // namespace

class MLNMapViewMetalRenderableResource final : public mbgl::mtl::RenderableResource {
public:
  MLNMapViewMetalRenderableResource(MLNMapViewMetalImpl& backend_)
      : backend(backend_), delegate([[MLNMapViewImplDelegate alloc] initWithImpl:&backend]) {}

  void bind() override {
    if (!commandQueue) {
      commandQueue = [mtlView.device newCommandQueue];
    }

    if (!commandBuffer) {
      commandBuffer = [commandQueue commandBuffer];
      commandBufferPtr = NS::RetainPtr((__bridge MTL::CommandBuffer*)commandBuffer);
    }
  }

  const mbgl::mtl::RendererBackend& getBackend() const override { return backend; }

  const mbgl::mtl::MTLCommandBufferPtr& getCommandBuffer() const override {
    return commandBufferPtr;
  }

  virtual mbgl::mtl::MTLBlitPassDescriptorPtr getUploadPassDescriptor() const override {
    // Create from render pass descriptor?
    return NS::TransferPtr(MTL::BlitPassDescriptor::alloc()->init());
  }

  const mbgl::mtl::MTLRenderPassDescriptorPtr& getRenderPassDescriptor() const override {
    if (!cachedRenderPassDescriptor) {
      auto* mtlDesc = mtlView.currentRenderPassDescriptor;
      cachedRenderPassDescriptor = NS::RetainPtr((__bridge MTL::RenderPassDescriptor*)mtlDesc);
    }
    return cachedRenderPassDescriptor;
  }

  void swap() override {
    id<CAMetalDrawable> currentDrawable = [mtlView currentDrawable];
    [commandBuffer presentDrawable:currentDrawable];
    [commandBuffer commit];

    // Un-comment for synchronous, which can help troubleshoot rendering problems,
    // particularly those related to resource tracking and multiple queued buffers.
    //[commandBuffer waitUntilCompleted];

    commandBuffer = nil;
    commandBufferPtr.reset();

    cachedRenderPassDescriptor.reset();
  }

  mbgl::Size framebufferSize() {
    assert(mtlView);
    return {static_cast<uint32_t>(mtlView.drawableSize.width),
            static_cast<uint32_t>(mtlView.drawableSize.height)};
  }

private:
  MLNMapViewMetalImpl& backend;
  mbgl::mtl::MTLCommandBufferPtr commandBufferPtr;
  mutable mbgl::mtl::MTLRenderPassDescriptorPtr cachedRenderPassDescriptor;

public:
  MLNMapViewImplDelegate* delegate = nil;
  MTKView* mtlView = nil;
  id<MTLCommandBuffer> commandBuffer;
  id<MTLCommandQueue> commandQueue;

  // We count how often the context was activated/deactivated so that we can truly deactivate it
  // after the activation count drops to 0.
  NSUInteger activationCount = 0;
};

MLNMapViewMetalImpl::MLNMapViewMetalImpl(MLNMapView* nativeView_)
    : MLNMapViewImpl(nativeView_),
      mbgl::mtl::RendererBackend(mbgl::gfx::ContextMode::Unique),
      mbgl::gfx::Renderable({0, 0}, std::make_unique<MLNMapViewMetalRenderableResource>(*this)) {
  auto& resource = getResource<MLNMapViewMetalRenderableResource>();
  if (resource.mtlView) {
    return;
  }

  id<MTLDevice> device = (__bridge id<MTLDevice>)resource.getBackend().getDevice().get();

  resource.mtlView = [[MTKView alloc] initWithFrame:mapView.bounds device:device];
  resource.mtlView.delegate = resource.delegate;
  resource.mtlView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  resource.mtlView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
  resource.mtlView.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  // MSAA is opt-in via MLNRendererConfiguration (default 1 = off, matching the historical
  // single-sampled path). When enabled, the MTKView renders into a multisampled target and
  // resolves into the drawable; pipelines pick their raster sample count up from the render
  // pass descriptor (see shaders/mtl/shader_program.cpp).
  const NSUInteger requestedSampleCount = MLNRendererConfiguration.currentConfiguration.sampleCount;
  if (requestedSampleCount > 1) {
    resource.mtlView.sampleCount = MLNSupportedMetalSampleCount(device, requestedSampleCount);
  }
  resource.mtlView.layer.opaque = mapView.opaque;
  resource.mtlView.enableSetNeedsDisplay = NO;
  CAMetalLayer* metalLayer = MLN_OBJC_DYNAMIC_CAST(resource.mtlView.layer, CAMetalLayer);
  metalLayer.presentsWithTransaction = presentsWithTransaction;

  [mapView addSubview:resource.mtlView positioned:NSWindowBelow relativeTo:nil];
}

MLNMapViewMetalImpl::~MLNMapViewMetalImpl() = default;

void MLNMapViewMetalImpl::activate() {
  auto& resource = getResource<MLNMapViewMetalRenderableResource>();
  if (resource.activationCount++) {
    return;
  }
}

void MLNMapViewMetalImpl::deactivate() {
  auto& resource = getResource<MLNMapViewMetalRenderableResource>();
  if (--resource.activationCount) {
    return;
  }
}

/// This function is called before we start rendering, when iOS invokes our rendering method.
/// iOS already sets the correct framebuffer and viewport for us, so we need to update the
/// context state with the anticipated values.
void MLNMapViewMetalImpl::updateAssumedState() {
  auto& resource = getResource<MLNMapViewMetalRenderableResource>();
  assumeFramebufferBinding(ImplicitFramebufferBinding);
  assumeViewport(0, 0, resource.framebufferSize());
}

mbgl::PremultipliedImage MLNMapViewMetalImpl::readStillImage() {
  // return readFramebuffer(mapView.framebufferSize); // TODO: RendererBackend::readFramebuffer
  return {};
}

MLNBackendResource* MLNMapViewMetalImpl::getObject() {
  auto& resource = getResource<MLNMapViewMetalRenderableResource>();

  return [[MLNBackendResource alloc] initWithMTKView:resource.mtlView
                                              device:resource.mtlView.device
                                renderPassDescriptor:resource.mtlView.currentRenderPassDescriptor
                                       commandBuffer:resource.commandBuffer];
}
