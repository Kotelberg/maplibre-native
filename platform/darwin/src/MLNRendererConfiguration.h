#import <Foundation/Foundation.h>
#import "MLNFoundation.h"

NS_ASSUME_NONNULL_BEGIN

/**
 The MLNRendererConfiguration object represents configuration values for the
 renderer.
 */
MLN_EXPORT
@interface MLNRendererConfiguration : NSObject

/** Returns an instance of the current renderer configuration. */
@property (class, nonatomic, readonly) MLNRendererConfiguration *currentConfiguration;

/** The scale factor to use.

 Based on the native scale where available, otherwise the standard screen scale. */
@property (nonatomic, readonly) const float scaleFactor;

/** The name of the font family to use for client-side text rendering of CJK ideographs.

 Set `MLNIdeographicFontFamilyName` in your containing application's Info.plist to
 font family name(s) that will be available at run time, such as “PingFang TC”
 or “Marker Felt”. This plist key accepts:

 - A string value of a single font family name.

 - An array of font family names. Fonts will be used in the defined order,
 eventually falling back to default system font if none are available.

 - A boolean value NO to disable client-side rendering of CJK glyphs —
 remote fonts specified in your style will be used instead.
 */
@property (nonatomic, readonly, nullable) NSString *localFontFamilyName;

- (nullable NSString *)localFontFamilyNameWithInfoDictionaryObject:
    (nullable id)infoDictionaryObject;

/**
 A Boolean value indicating whether symbol layers may enable per-source symbol
 collision detection.

 Set `MLNCollisionBehaviorPre4_0` in your containing app's Info.plist or by using
 `[[NSUserDefaults standardUserDefaults] setObject:@(YES) forKey:@"MLNCollisionBehaviorPre4_0"]`.
 If both are set, the value from `NSUserDefaults` takes priority.

 Setting this property to `YES` in the plist results in symbol layers only running
 collision detection against other symbol layers that are part of the same source.
 */
@property (nonatomic, readonly) BOOL perSourceCollisions;

- (BOOL)perSourceCollisionsWithInfoDictionaryObject:(nullable id)infoDictionaryObject;

/**
 The number of samples used for multisample anti-aliasing (MSAA) of the map surface, on
 platforms that render with Metal.

 Set `MLNRendererSampleCount` in your containing application's Info.plist to an integer
 sample count. Supported values are `1`, `2`, `4`, and `8`; any other value is rounded down
 to the nearest supported value, with a minimum of `1`. The default is `1`, which disables
 MSAA and matches the historical, single-sampled rendering path.

 Higher sample counts smooth jagged edges on thin extrusion silhouettes and 3D models at the
 cost of additional GPU memory and rendering time. The value is further reduced to the
 highest sample count the current `MTLDevice` actually supports.
 */
@property (nonatomic, readonly) NSUInteger sampleCount;

- (NSUInteger)sampleCountWithInfoDictionaryObject:(nullable id)infoDictionaryObject;

@end

NS_ASSUME_NONNULL_END
