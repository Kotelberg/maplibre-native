#include <mbgl/style/layers/model_layer_impl.hpp>

namespace mbgl {
namespace style {

bool ModelLayer::Impl::hasLayoutDifference(const Layer::Impl& other) const {
    assert(other.getTypeInfo() == getTypeInfo());
    const auto& impl = static_cast<const ModelLayer::Impl&>(other);
    return modelId != impl.modelId;
}

void ModelLayer::Impl::stringifyLayout(rapidjson::Writer<rapidjson::StringBuffer>&) const {}

} // namespace style
} // namespace mbgl
