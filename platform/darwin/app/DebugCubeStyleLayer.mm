#import "DebugCubeStyleLayer.h"
#import "MLNCustomDrawableStyleLayer.h"

#include <mbgl/style/layer.hpp>
#include <mbgl/style/layers/custom_drawable_layer.hpp>
#include <mbgl/style/layers/debug_cube_layer_host.hpp>

#include <memory>

@interface MLNCustomDrawableStyleLayer (Internal)
- (instancetype)initWithPendingLayer:(std::unique_ptr<mbgl::style::Layer>)pendingLayer;
@end

@implementation DebugCubeStyleLayer

- (instancetype)initWithIdentifier:(NSString *)identifier {
  auto layer = std::make_unique<mbgl::style::CustomDrawableLayer>(
      identifier.UTF8String,
      std::make_unique<mbgl::style::DebugCubeLayerHost>(mbgl::LatLng{50.4501, 30.5234}, 50.0));
  return self = [super initWithPendingLayer:std::move(layer)];
}

@end
