#import <Foundation/Foundation.h>

#import "MLNFoundation.h"
#import "MLNStyleLayer.h"

NS_ASSUME_NONNULL_BEGIN

/**
 An `MLNModelStyleLayer` renders one glTF/GLB model per `Point` feature of a
 GeoJSON source (fork extension, not part of the MapLibre style spec).

 Per-feature placement is data-driven through feature properties:

 - `bearing` — yaw rotation in degrees (clockwise from north)
 - `size` — model height in meters
 - `footprint` — optional x/y multiplier applied on top of `size` (default 1)

 Register GLB assets through `modelAssets` and select one with `modelID`.
 */
MLN_EXPORT
@interface MLNModelStyleLayer : MLNStyleLayer

- (instancetype)initWithIdentifier:(NSString *)identifier
                  sourceIdentifier:(NSString *)sourceIdentifier;

/**
 Maps asset identifiers to local GLB file paths.
 */
@property (nonatomic, copy) NSDictionary<NSString *, NSString *> *modelAssets;

/**
 The asset identifier (a key of `modelAssets`) rendered for features. Features
 render the placeholder cube while this is unset or unresolved.
 */
@property (nonatomic, copy, nullable) NSString *modelID;

@end

NS_ASSUME_NONNULL_END
