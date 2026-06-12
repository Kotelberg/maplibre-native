#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>

#include <mbgl/style/conversion/property_value.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/style/layer_observer.hpp>

namespace mbgl {
namespace style {

namespace {
const LayerTypeInfo typeInfoModel{.type = "model",
                                  .source = LayerTypeInfo::Source::Required,
                                  .pass3d = LayerTypeInfo::Pass3D::NotRequired,
                                  .layout = LayerTypeInfo::Layout::NotRequired,
                                  .fadingTiles = LayerTypeInfo::FadingTiles::NotRequired,
                                  .crossTileIndex = LayerTypeInfo::CrossTileIndex::NotRequired,
                                  // Geometry: GeoJSONSource::supportsLayerType requires it; the
                                  // render layer ignores tiles and reads the source directly (M3a).
                                  .tileKind = LayerTypeInfo::TileKind::Geometry};
} // namespace

const LayerTypeInfo* ModelLayer::Impl::staticTypeInfo() noexcept {
    return &typeInfoModel;
}

ModelLayer::ModelLayer(const std::string& layerID, const std::string& sourceID)
    : Layer(makeMutable<Impl>(layerID, sourceID)) {}

ModelLayer::ModelLayer(Immutable<Impl> impl_)
    : Layer(std::move(impl_)) {}

ModelLayer::~ModelLayer() = default;

const ModelLayer::Impl& ModelLayer::impl() const {
    return static_cast<const Impl&>(*baseImpl);
}

Mutable<ModelLayer::Impl> ModelLayer::mutableImpl() const {
    return makeMutable<Impl>(impl());
}

Mutable<Layer::Impl> ModelLayer::mutableBaseImpl() const {
    return staticMutableCast<Layer::Impl>(mutableImpl());
}

std::unique_ptr<Layer> ModelLayer::cloneRef(const std::string& id_) const {
    auto impl_ = mutableImpl();
    impl_->id = id_;
    return std::make_unique<ModelLayer>(std::move(impl_));
}

// Layout: model-id

PropertyValue<std::string> ModelLayer::getDefaultModelId() {
    return {};
}

const PropertyValue<std::string>& ModelLayer::getModelId() const {
    return impl().modelId;
}

void ModelLayer::setModelId(const PropertyValue<std::string>& value) {
    if (value == getModelId()) return;
    auto impl_ = mutableImpl();
    impl_->modelId = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

// Paint: model-scale (meters)

PropertyValue<float> ModelLayer::getDefaultModelScale() {
    return {20.0f};
}

const PropertyValue<float>& ModelLayer::getModelScale() const {
    return impl().modelScale;
}

void ModelLayer::setModelScale(const PropertyValue<float>& value) {
    if (value == getModelScale()) return;
    auto impl_ = mutableImpl();
    impl_->modelScale = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

// Paint: model-rotation (yaw degrees)

PropertyValue<float> ModelLayer::getDefaultModelRotation() {
    return {0.0f};
}

const PropertyValue<float>& ModelLayer::getModelRotation() const {
    return impl().modelRotation;
}

void ModelLayer::setModelRotation(const PropertyValue<float>& value) {
    if (value == getModelRotation()) return;
    auto impl_ = mutableImpl();
    impl_->modelRotation = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

// Paint: model-opacity

PropertyValue<float> ModelLayer::getDefaultModelOpacity() {
    return {1.0f};
}

const PropertyValue<float>& ModelLayer::getModelOpacity() const {
    return impl().modelOpacity;
}

void ModelLayer::setModelOpacity(const PropertyValue<float>& value) {
    if (value == getModelOpacity()) return;
    auto impl_ = mutableImpl();
    impl_->modelOpacity = value;
    baseImpl = std::move(impl_);
    observer->onLayerChanged(*this);
}

using namespace conversion;

std::optional<Error> ModelLayer::setPropertyInternal(const std::string& name, const Convertible& value) {
    Error error;

    if (name == "model-id") {
        const auto property = convert<PropertyValue<std::string>>(value, error, /*allowDataExpressions=*/true, false);
        if (!property) return error;
        setModelId(*property);
        return std::nullopt;
    }
    if (name == "model-scale") {
        const auto property = convert<PropertyValue<float>>(value, error, /*allowDataExpressions=*/true, false);
        if (!property) return error;
        setModelScale(*property);
        return std::nullopt;
    }
    if (name == "model-rotation") {
        const auto property = convert<PropertyValue<float>>(value, error, /*allowDataExpressions=*/true, false);
        if (!property) return error;
        setModelRotation(*property);
        return std::nullopt;
    }
    if (name == "model-opacity") {
        const auto property = convert<PropertyValue<float>>(value, error, /*allowDataExpressions=*/false, false);
        if (!property) return error;
        setModelOpacity(*property);
        return std::nullopt;
    }

    return Error{"layer doesn't support this property"};
}

StyleProperty ModelLayer::getProperty(const std::string& name) const {
    if (name == "model-id") return makeStyleProperty(getModelId());
    if (name == "model-scale") return makeStyleProperty(getModelScale());
    if (name == "model-rotation") return makeStyleProperty(getModelRotation());
    if (name == "model-opacity") return makeStyleProperty(getModelOpacity());
    return {};
}

} // namespace style
} // namespace mbgl
