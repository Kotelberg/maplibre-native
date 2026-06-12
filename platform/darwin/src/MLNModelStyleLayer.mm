#import "MLNModelStyleLayer.h"

#import "MLNModelStyleLayer_Private.h"
#import "MLNStyleLayer_Private.h"

#include <mbgl/style/expression/dsl.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/property_expression.hpp>

#include <map>
#include <memory>
#include <string>

@implementation MLNModelStyleLayer

- (instancetype)initWithIdentifier:(NSString *)identifier
                  sourceIdentifier:(NSString *)sourceIdentifier {
  namespace dsl = mbgl::style::expression::dsl;
  auto layer = std::make_unique<mbgl::style::ModelLayer>(identifier.UTF8String,
                                                         sourceIdentifier.UTF8String);
  // Per-feature data-driven placement (missing property -> evaluation default).
  layer->setModelRotation(
      mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("bearing"))));
  layer->setModelScale(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("size"))));
  layer->setModelFootprint(
      mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("footprint"))));
  return self = [super initWithPendingLayer:std::move(layer)];
}

- (mbgl::style::ModelLayer *)rawModelLayer {
  return static_cast<mbgl::style::ModelLayer *>(self.rawLayer);
}

- (void)setModelAssets:(NSDictionary<NSString *, NSString *> *)modelAssets {
  std::map<std::string, std::string> assets;
  for (NSString *assetID in modelAssets) {
    assets.emplace(assetID.UTF8String, modelAssets[assetID].UTF8String);
  }
  self.rawModelLayer->setModelAssets(std::move(assets));
}

- (NSDictionary<NSString *, NSString *> *)modelAssets {
  NSMutableDictionary<NSString *, NSString *> *assets = [NSMutableDictionary dictionary];
  for (const auto &entry : self.rawModelLayer->getModelAssets()) {
    assets[@(entry.first.c_str())] = @(entry.second.c_str());
  }
  return assets;
}

- (void)setModelID:(NSString *)modelID {
  if (modelID) {
    self.rawModelLayer->setModelId(
        mbgl::style::PropertyValue<std::string>(std::string(modelID.UTF8String)));
  } else {
    self.rawModelLayer->setModelId(mbgl::style::PropertyValue<std::string>());
  }
}

- (NSString *)modelID {
  const auto &value = self.rawModelLayer->getModelId();
  if (value.isConstant()) {
    return @(value.asConstant().c_str());
  }
  return nil;
}

@end

namespace mbgl {

MLNStyleLayer *ModelStyleLayerPeerFactory::createPeer(style::Layer *rawLayer) {
  return [[MLNModelStyleLayer alloc] initWithRawLayer:rawLayer];
}

}  // namespace mbgl
