#import "Mapbox.h"

// M6 demo: experimental `model` style layer rendering a GLB via Filament.
// Reads per-feature `bearing`/`size` properties for rotation and scale.
@interface ModelDemoStyleLayer : MLNCustomDrawableStyleLayer

- (instancetype)initWithIdentifier:(NSString *)identifier
                          sourceID:(NSString *)sourceID
                           glbPath:(NSString *)glbPath;

@end
