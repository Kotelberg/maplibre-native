#pragma once

#include <mbgl/style/light.hpp>
#include <mbgl/style/property_value.hpp>
#include <mbgl/style/types.hpp>
#include <mbgl/style/position.hpp>
#include <mbgl/style/properties.hpp>
#include <mbgl/renderer/property_evaluator.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/util/indexed_tuple.hpp>

namespace mbgl {
namespace style {

template <class T>
class LightProperty {
public:
    using TransitionableType = Transitionable<PropertyValue<T>>;
    using UnevaluatedType = Transitioning<PropertyValue<T>>;
    using EvaluatorType = PropertyEvaluator<T>;
    using PossiblyEvaluatedType = T;
    using Type = T;
    static constexpr bool IsDataDriven = false;
    static constexpr bool IsOverridable = false;
};

struct LightAnchor : LightProperty<LightAnchorType> {
    static LightAnchorType defaultValue() { return LightAnchorType::Viewport; }
};

struct LightPosition : LightProperty<Position> {
    static Position defaultValue() {
        std::array<float, 3> default_ = {{1.15f, 210.f, 30.f}};
        return Position{{default_}};
    }
};

struct LightColor : LightProperty<Color> {
    static Color defaultValue() { return Color::white(); }
};

struct LightIntensity : LightProperty<float> {
    static float defaultValue() { return 0.5; }
};

// Fork-local directional-shadow light properties (SHADOW_REWRITE_DESIGN.md §3.1). Not part of the
// public MapLibre style spec — Mapbox's shape is a `lights` array of typed entries. We extend the
// legacy root `light` object with a lenient parser + evaluated state so the renderer-owned
// ShadowPass and its receivers can read shadow enable/darkness from the scene's light.
struct LightCastShadows : LightProperty<bool> {
    // Default true: preserves the pre-P1 behavior (shadows on under the Metal+env gate). A style can
    // opt out with `"cast-shadows": false`; MLN_RENDER_3D_ENHANCEMENTS=0 remains a hard kill-switch.
    static bool defaultValue() { return true; }
};

struct LightShadowIntensity : LightProperty<float> {
    // Default 0.32 = the user-approved "subtle" darkness (matches the prior hardcoded value).
    static float defaultValue() { return 0.32f; }
};

using LightProperties =
    Properties<LightAnchor, LightPosition, LightColor, LightIntensity, LightCastShadows, LightShadowIntensity>;

class Light::Impl {
public:
    LightProperties::Transitionable properties;
};

} // namespace style
} // namespace mbgl
