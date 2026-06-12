#import "ModelDemoStyleLayer.h"
#import "MLNCustomDrawableStyleLayer.h"

#include <mbgl/style/expression/dsl.hpp>
#include <mbgl/style/layer.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/property_expression.hpp>

#include <memory>

@interface MLNCustomDrawableStyleLayer (Internal)
- (instancetype)initWithPendingLayer:(std::unique_ptr<mbgl::style::Layer>)pendingLayer;
@end

@implementation ModelDemoStyleLayer

- (instancetype)initWithIdentifier:(NSString *)identifier
                          sourceID:(NSString *)sourceID
                           glbPath:(NSString *)glbPath {
  namespace dsl = mbgl::style::expression::dsl;
  auto layer =
      std::make_unique<mbgl::style::ModelLayer>(identifier.UTF8String, sourceID.UTF8String);
  layer->setModelAssets({{"demo", glbPath.UTF8String}});
  layer->setModelId(std::string("demo"));
  layer->setModelRotation(
      mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("bearing"))));
  layer->setModelScale(mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("size"))));
  layer->setModelFootprint(
      mbgl::style::PropertyExpression<float>(dsl::number(dsl::get("footprint"))));
  return self = [super initWithPendingLayer:std::move(layer)];
}

@end
