#import "MLNRendererConfiguration.h"

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

static NSString * const MLNCollisionBehaviorPre4_0Key = @"MLNCollisionBehaviorPre4_0";
static NSString * const MLNIdeographicFontFamilyNameKey = @"MLNIdeographicFontFamilyName";
static NSString * const MLNRendererSampleCountKey = @"MLNRendererSampleCount";

@implementation MLNRendererConfiguration

+ (instancetype)currentConfiguration {
    return [[self alloc] init];
}

- (const float)scaleFactor {
#if TARGET_OS_IPHONE
    return [UIScreen instancesRespondToSelector:@selector(nativeScale)] ? [[UIScreen mainScreen] nativeScale] : [[UIScreen mainScreen] scale];
#else
    return [NSScreen mainScreen].backingScaleFactor;
#endif
}

- (nullable NSString *)localFontFamilyName {
    id infoDictionaryObject = [NSBundle.mainBundle objectForInfoDictionaryKey:MLNIdeographicFontFamilyNameKey];
    return [self localFontFamilyNameWithInfoDictionaryObject:infoDictionaryObject];
}

- (nullable NSString *)localFontFamilyNameWithInfoDictionaryObject:(nullable id)infoDictionaryObject {
    if ([infoDictionaryObject isKindOfClass:[NSNumber class]] && ![infoDictionaryObject boolValue]) {
        // NO means don’t use local fonts.
        return nil;
    } else if ([infoDictionaryObject isKindOfClass:[NSString class]]) {
        return infoDictionaryObject;
    } else if ([infoDictionaryObject isKindOfClass:[NSArray class]]) {
        // mbgl::LocalGlyphRasterizer::Impl accepts only a single string, but form a cascade list with one font on each line.
        return [infoDictionaryObject componentsJoinedByString:@"\n"];
    }

#if TARGET_OS_IPHONE
    return [UIFont systemFontOfSize:0 weight:UIFontWeightRegular].familyName;
#else
    return [NSFont systemFontOfSize:0 weight:NSFontWeightRegular].familyName;
#endif
}

- (BOOL)perSourceCollisions {
    id infoDictionaryObject = [NSBundle.mainBundle objectForInfoDictionaryKey:MLNCollisionBehaviorPre4_0Key];
    return [self perSourceCollisionsWithInfoDictionaryObject:infoDictionaryObject];
}

- (BOOL)perSourceCollisionsWithInfoDictionaryObject:(nullable id)infoDictionaryObject {
    // Set the collision behaviour. A value set in `NSUserDefaults.standardUserDefaults`
    // should override anything in the application's info.plist
    if ([NSUserDefaults.standardUserDefaults objectForKey:MLNCollisionBehaviorPre4_0Key]) {
        return [NSUserDefaults.standardUserDefaults boolForKey:MLNCollisionBehaviorPre4_0Key];
    } else if ([infoDictionaryObject isKindOfClass:[NSNumber class]] || [infoDictionaryObject isKindOfClass:[NSString class]]) {
        // Also support NSString to correspond with the behavior of `-[NSUserDefaults boolForKey:]`
        return [infoDictionaryObject boolValue];
    }
    return NO;
}

- (NSUInteger)sampleCount {
    id infoDictionaryObject = [NSBundle.mainBundle objectForInfoDictionaryKey:MLNRendererSampleCountKey];
    return [self sampleCountWithInfoDictionaryObject:infoDictionaryObject];
}

- (NSUInteger)sampleCountWithInfoDictionaryObject:(nullable id)infoDictionaryObject {
    // Supported MSAA sample counts, highest first, so we can round an arbitrary
    // requested value down to the nearest one we support (minimum 1, which disables MSAA).
    static const NSUInteger supportedSampleCounts[] = {8, 4, 2, 1};

    if (![infoDictionaryObject isKindOfClass:[NSNumber class]]) {
        return 1;
    }

    NSInteger requestedSampleCount = [infoDictionaryObject integerValue];
    for (NSUInteger i = 0; i < sizeof(supportedSampleCounts) / sizeof(supportedSampleCounts[0]); i++) {
        if (requestedSampleCount >= (NSInteger)supportedSampleCounts[i]) {
            return supportedSampleCounts[i];
        }
    }
    return 1;
}

@end
