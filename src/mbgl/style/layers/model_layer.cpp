// clang-format off

// This file is generated. Edit scripts/generate-style-code.js, then run `make style-code`.

#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>
#include <mbgl/style/layer_observer.hpp>
#include <mbgl/style/conversion/color_ramp_property_value.hpp>
#include <mbgl/style/conversion/constant.hpp>
#include <mbgl/style/conversion/property_value.hpp>
#include <mbgl/style/conversion/transition_options.hpp>
#include <mbgl/style/conversion/json.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/util/traits.hpp>

#include <mapbox/eternal.hpp>

namespace mbgl {
namespace style {


// static
const LayerTypeInfo* ModelLayer::Impl::staticTypeInfo() noexcept {
    const static LayerTypeInfo typeInfo{.type="model",
                                        .source=LayerTypeInfo::Source::Required,
                                        .pass3d=LayerTypeInfo::Pass3D::NotRequired,
                                        .layout=LayerTypeInfo::Layout::NotRequired,
                                        .fadingTiles=LayerTypeInfo::FadingTiles::NotRequired,
                                        .crossTileIndex=LayerTypeInfo::CrossTileIndex::NotRequired,
                                        .tileKind=LayerTypeInfo::TileKind::Geometry};
    return &typeInfo;
}

ModelLayer::ModelLayer(const std::string& layerID, const std::string& sourceID)
    : Layer(makeMutable<Impl>(layerID, sourceID)) {
}

ModelLayer::ModelLayer(Immutable<Impl> impl_)
    : Layer(std::move(impl_)) {
}

ModelLayer::~ModelLayer() {
    weakFactory.invalidateWeakPtrs();
}

const ModelLayer::Impl& ModelLayer::impl() const {
    return static_cast<const Impl&>(*baseImpl);
}

Mutable<ModelLayer::Impl> ModelLayer::mutableImpl() const {
    return makeMutable<Impl>(impl());
}

std::unique_ptr<Layer> ModelLayer::cloneRef(const std::string& id_) const {
    auto impl_ = mutableImpl();
    impl_->id = id_;
    impl_->paint = ModelPaintProperties::Transitionable();
    return std::make_unique<ModelLayer>(std::move(impl_));
}

void ModelLayer::Impl::stringifyLayout(rapidjson::Writer<rapidjson::StringBuffer>& writer) const {
    layout.stringify(writer);
}

// Layout properties

PropertyValue<std::string> ModelLayer::getDefaultModelId() {
    return ModelId::defaultValue();
}

const PropertyValue<std::string>& ModelLayer::getModelId() const {
    return impl().layout.get<ModelId>();
}

void ModelLayer::setModelId(const PropertyValue<std::string>& value) {
    if (value == getModelId()) return;
    auto impl_ = mutableImpl();
    impl_->layout.get<ModelId>() = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

// Paint properties

PropertyValue<float> ModelLayer::getDefaultModelFootprint() {
    return {1.f};
}

const PropertyValue<float>& ModelLayer::getModelFootprint() const {
    return impl().paint.template get<ModelFootprint>().value;
}

void ModelLayer::setModelFootprint(const PropertyValue<float>& value) {
    if (value == getModelFootprint())
        return;
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelFootprint>().value = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

void ModelLayer::setModelFootprintTransition(const TransitionOptions& options) {
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelFootprint>().options = options;
    baseImpl = std::move(impl_);
}

TransitionOptions ModelLayer::getModelFootprintTransition() const {
    return impl().paint.template get<ModelFootprint>().options;
}

PropertyValue<float> ModelLayer::getDefaultModelOpacity() {
    return {1.f};
}

const PropertyValue<float>& ModelLayer::getModelOpacity() const {
    return impl().paint.template get<ModelOpacity>().value;
}

void ModelLayer::setModelOpacity(const PropertyValue<float>& value) {
    if (value == getModelOpacity())
        return;
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelOpacity>().value = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

void ModelLayer::setModelOpacityTransition(const TransitionOptions& options) {
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelOpacity>().options = options;
    baseImpl = std::move(impl_);
}

TransitionOptions ModelLayer::getModelOpacityTransition() const {
    return impl().paint.template get<ModelOpacity>().options;
}

PropertyValue<float> ModelLayer::getDefaultModelRotation() {
    return {0.f};
}

const PropertyValue<float>& ModelLayer::getModelRotation() const {
    return impl().paint.template get<ModelRotation>().value;
}

void ModelLayer::setModelRotation(const PropertyValue<float>& value) {
    if (value == getModelRotation())
        return;
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelRotation>().value = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

void ModelLayer::setModelRotationTransition(const TransitionOptions& options) {
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelRotation>().options = options;
    baseImpl = std::move(impl_);
}

TransitionOptions ModelLayer::getModelRotationTransition() const {
    return impl().paint.template get<ModelRotation>().options;
}

PropertyValue<float> ModelLayer::getDefaultModelScale() {
    return {20.f};
}

const PropertyValue<float>& ModelLayer::getModelScale() const {
    return impl().paint.template get<ModelScale>().value;
}

void ModelLayer::setModelScale(const PropertyValue<float>& value) {
    if (value == getModelScale())
        return;
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelScale>().value = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

void ModelLayer::setModelScaleTransition(const TransitionOptions& options) {
    auto impl_ = mutableImpl();
    impl_->paint.template get<ModelScale>().options = options;
    baseImpl = std::move(impl_);
}

TransitionOptions ModelLayer::getModelScaleTransition() const {
    return impl().paint.template get<ModelScale>().options;
}

// Model asset registry (runtime SDK API; not part of the style spec). Mirrors
// the image-registry precedent (Style::addImage): the style references models by
// id through the `model-id` property, and the host application registers the
// id -> local glTF/GLB path mapping at runtime. Mutating it notifies observers so
// the render tree can pick up newly registered assets.
const std::map<std::string, std::string>& ModelLayer::getModelAssets() const {
    return impl().modelAssets;
}

void ModelLayer::setModelAssets(std::map<std::string, std::string> assets) {
    if (assets == getModelAssets()) return;
    auto impl_ = mutableImpl();
    impl_->modelAssets = std::move(assets);
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

using namespace conversion;

namespace {

constexpr uint8_t kPaintPropertyCount = 8u;

enum class Property : uint8_t {
    ModelFootprint,
    ModelOpacity,
    ModelRotation,
    ModelScale,
    ModelFootprintTransition,
    ModelOpacityTransition,
    ModelRotationTransition,
    ModelScaleTransition,
    ModelId = kPaintPropertyCount,
};

template <typename T>
constexpr uint8_t toUint8(T t) noexcept {
    return uint8_t(mbgl::underlying_type(t));
}

constexpr const auto layerProperties = mapbox::eternal::hash_map<mapbox::eternal::string, uint8_t>(
    {{"model-footprint", toUint8(Property::ModelFootprint)},
     {"model-opacity", toUint8(Property::ModelOpacity)},
     {"model-rotation", toUint8(Property::ModelRotation)},
     {"model-scale", toUint8(Property::ModelScale)},
     {"model-footprint-transition", toUint8(Property::ModelFootprintTransition)},
     {"model-opacity-transition", toUint8(Property::ModelOpacityTransition)},
     {"model-rotation-transition", toUint8(Property::ModelRotationTransition)},
     {"model-scale-transition", toUint8(Property::ModelScaleTransition)},
     {"model-id", toUint8(Property::ModelId)}});

StyleProperty getLayerProperty(const ModelLayer& layer, Property property) {
    switch (property) {
        case Property::ModelFootprint:
            return makeStyleProperty(layer.getModelFootprint());
        case Property::ModelOpacity:
            return makeStyleProperty(layer.getModelOpacity());
        case Property::ModelRotation:
            return makeStyleProperty(layer.getModelRotation());
        case Property::ModelScale:
            return makeStyleProperty(layer.getModelScale());
        case Property::ModelFootprintTransition:
            return makeStyleProperty(layer.getModelFootprintTransition());
        case Property::ModelOpacityTransition:
            return makeStyleProperty(layer.getModelOpacityTransition());
        case Property::ModelRotationTransition:
            return makeStyleProperty(layer.getModelRotationTransition());
        case Property::ModelScaleTransition:
            return makeStyleProperty(layer.getModelScaleTransition());
        case Property::ModelId:
            return makeStyleProperty(layer.getModelId());
    }
    return {};
}

StyleProperty getLayerProperty(const ModelLayer& layer, const std::string& name) {
    const auto it = layerProperties.find(name.c_str());
    if (it == layerProperties.end()) {
        return {};
    }
    return getLayerProperty(layer, static_cast<Property>(it->second));
}

} // namespace

Value ModelLayer::serialize() const {
    auto result = Layer::serialize();
    assert(result.getObject());
    for (const auto& property : layerProperties) {
        auto styleProperty = getLayerProperty(*this, static_cast<Property>(property.second));
        if (styleProperty.getKind() == StyleProperty::Kind::Undefined) continue;
        serializeProperty(result, styleProperty, property.first.c_str(), property.second < kPaintPropertyCount);
    }
    return result;
}

std::optional<Error> ModelLayer::setPropertyInternal(const std::string& name, const Convertible& value) {
    const auto it = layerProperties.find(name.c_str());
    if (it == layerProperties.end()) return Error{"layer doesn't support this property"};

    auto property = static_cast<Property>(it->second);

    if (property == Property::ModelFootprint || property == Property::ModelRotation ||
        property == Property::ModelScale) {
        Error error;
        const auto& typedValue = convert<PropertyValue<float>>(value, error, true, false);
        if (!typedValue) {
            return error;
        }

        if (property == Property::ModelFootprint) {
            setModelFootprint(*typedValue);
            return std::nullopt;
        }

        if (property == Property::ModelRotation) {
            setModelRotation(*typedValue);
            return std::nullopt;
        }

        if (property == Property::ModelScale) {
            setModelScale(*typedValue);
            return std::nullopt;
        }
    }
    if (property == Property::ModelOpacity) {
        Error error;
        const auto& typedValue = convert<PropertyValue<float>>(value, error, false, false);
        if (!typedValue) {
            return error;
        }

        setModelOpacity(*typedValue);
        return std::nullopt;
    }
    if (property == Property::ModelId) {
        Error error;
        const auto& typedValue = convert<PropertyValue<std::string>>(value, error, true, false);
        if (!typedValue) {
            return error;
        }

        setModelId(*typedValue);
        return std::nullopt;
    }

    Error error;
    std::optional<TransitionOptions> transition = convert<TransitionOptions>(value, error);
    if (!transition) {
        return error;
    }

    if (property == Property::ModelFootprintTransition) {
        setModelFootprintTransition(*transition);
        return std::nullopt;
    }

    if (property == Property::ModelOpacityTransition) {
        setModelOpacityTransition(*transition);
        return std::nullopt;
    }

    if (property == Property::ModelRotationTransition) {
        setModelRotationTransition(*transition);
        return std::nullopt;
    }

    if (property == Property::ModelScaleTransition) {
        setModelScaleTransition(*transition);
        return std::nullopt;
    }

    return Error{"layer doesn't support this property"};
}

StyleProperty ModelLayer::getProperty(const std::string& name) const {
    return getLayerProperty(*this, name);
}

Mutable<Layer::Impl> ModelLayer::mutableBaseImpl() const {
    return staticMutableCast<Layer::Impl>(mutableImpl());
}

} // namespace style
} // namespace mbgl

// clang-format on
