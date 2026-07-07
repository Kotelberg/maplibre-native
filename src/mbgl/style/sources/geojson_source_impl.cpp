#include <mbgl/style/sources/geojson_source_impl.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/feature.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/thread_pool.hpp>
#include <mbgl/util/identity.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include <mapbox/geojsonvt.hpp>
#include <supercluster.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <cmath>
#include <memory>
#include <mutex>

namespace mbgl {
namespace style {

class GeoJSONVTData final : public GeoJSONData {
    void getTile(const CanonicalTileID& id, const std::function<void(TileFeatures)>& fn, bool runSynchronously) final {
        assert(fn);
        // mapbox::geojsonvt::GeoJSONVT::getTile() is NOT const — it lazily splits and memoises tiles into
        // an internal unordered_map, so two concurrent calls race that cache. The async path below hops
        // every call onto the single sequenced worker, which is why `impl` was documented "Accessed on
        // worker thread". The synchronous path runs on the CALLER's thread; a layer whose update() walks
        // placement synchronously (e.g. one driven by a per-frame render-thread callback) can invoke it on
        // the RENDER thread, concurrently with the worker still loading/tearing down this source's own
        // GeoJSONTiles — corrupting the geojson-vt cache and crashing later in vt_feature teardown
        // (SIGSEGV: render-thread and worker-thread tile-vector teardown both faulted releasing a
        // shared_count inside the same GeoJSONVTData). Serialize ALL impl access (sync + async) on a
        // shared mutex so the synchronous reader can never overlap the worker. The mutex is held by
        // shared_ptr so the async task, which may outlive `this`, keeps it alive.
        if (runSynchronously) {
            TileFeatures features;
            {
                std::lock_guard<std::mutex> lock(*implMutex);
                features = this->impl->getTile(id.z, id.x, id.y).features;
            }
            fn(std::move(features));
        } else {
            sequencedScheduler->scheduleAndReplyValue(
                util::SimpleIdentity::Empty,
                [id, geoJSONVT_impl = this->impl, mutex = this->implMutex]() -> TileFeatures {
                    std::lock_guard<std::mutex> lock(*mutex);
                    return geoJSONVT_impl->getTile(id.z, id.x, id.y).features;
                },
                fn);
        }
    }

    Features getChildren(const std::uint32_t) final { return {}; }

    Features getLeaves(const std::uint32_t, const std::uint32_t, const std::uint32_t) final { return {}; }

    std::uint8_t getClusterExpansionZoom(std::uint32_t) final { return 0; }

    friend GeoJSONData;
    GeoJSONVTData(const GeoJSON& geoJSON,
                  const mapbox::geojsonvt::Options& options,
                  std::shared_ptr<Scheduler> sequencedScheduler_)
        : impl(std::make_shared<mapbox::geojsonvt::GeoJSONVT>(geoJSON, options)),
          sequencedScheduler(std::move(sequencedScheduler_)) {
        assert(sequencedScheduler);
    }

    std::shared_ptr<mapbox::geojsonvt::GeoJSONVT> impl; // Mutated on every getTile(); guarded by implMutex.
    std::shared_ptr<Scheduler> sequencedScheduler;
    // Serializes impl->getTile() across the sequenced worker and any synchronous caller (the model
    // layer's render-thread placement walk). shared_ptr because the async task may outlive `this`.
    std::shared_ptr<std::mutex> implMutex = std::make_shared<std::mutex>();
};

class SuperclusterData final : public GeoJSONData {
    void getTile(const CanonicalTileID& id, const std::function<void(TileFeatures)>& fn, bool) final {
        assert(fn);
        fn(impl.getTile(id.z, id.x, id.y));
    }

    Features getChildren(const std::uint32_t cluster_id) final { return impl.getChildren(cluster_id); }

    Features getLeaves(const std::uint32_t cluster_id, const std::uint32_t limit, const std::uint32_t offset) final {
        return impl.getLeaves(cluster_id, limit, offset);
    }

    std::uint8_t getClusterExpansionZoom(std::uint32_t cluster_id) final {
        return impl.getClusterExpansionZoom(cluster_id);
    }

    friend GeoJSONData;
    SuperclusterData(const Features& features, const mapbox::supercluster::Options& options)
        : impl(features, options) {}
    mapbox::supercluster::Supercluster impl;
};

template <class T>
T evaluateFeature(const mapbox::feature::feature<double>& f,
                  const std::shared_ptr<expression::Expression>& expression,
                  std::optional<T> accumulated = std::nullopt) {
    const expression::EvaluationResult result = expression->evaluate(accumulated, f);
    if (result) {
        std::optional<T> typed = expression::fromExpressionValue<T>(*result);
        if (typed) {
            return std::move(*typed);
        }
    }
    return T();
}

// static
std::shared_ptr<GeoJSONData> GeoJSONData::create(const GeoJSON& geoJSON,
                                                 std::shared_ptr<Scheduler> sequencedScheduler,
                                                 const Immutable<GeoJSONOptions>& options) {
    constexpr double scale = util::EXTENT / util::tileSize_D;
    if (options->cluster && geoJSON.is<Features>() && !geoJSON.get<Features>().empty()) {
        mapbox::supercluster::Options clusterOptions;
        clusterOptions.maxZoom = options->clusterMaxZoom;
        clusterOptions.extent = util::EXTENT;
        clusterOptions.radius = static_cast<uint16_t>(::round(scale * options->clusterRadius));
        clusterOptions.minPoints = options->clusterMinPoints;

        auto feature = std::make_shared<Feature>();
        clusterOptions.map = [feature, options](const PropertyMap& properties) -> PropertyMap {
            PropertyMap ret{};
            if (properties.empty()) return ret;
            for (const auto& p : options->clusterProperties) {
                feature->properties = properties;
                ret[p.first] = evaluateFeature<Value>(*feature, p.second.first);
            }
            return ret;
        };
        clusterOptions.reduce = [feature, options](PropertyMap& toReturn, const PropertyMap& toFill) {
            for (const auto& p : options->clusterProperties) {
                if (!toFill.contains(p.first)) {
                    continue;
                }
                feature->properties = toFill;
                std::optional<Value> accumulated(toReturn[p.first]);
                toReturn[p.first] = evaluateFeature<Value>(*feature, p.second.second, accumulated);
            }
        };
        return std::shared_ptr<GeoJSONData>(new SuperclusterData(geoJSON.get<Features>(), clusterOptions));
    }

    mapbox::geojsonvt::Options vtOptions;
    vtOptions.maxZoom = options->maxzoom;
    vtOptions.extent = util::EXTENT;
    vtOptions.buffer = static_cast<uint16_t>(::round(scale * options->buffer));
    vtOptions.tolerance = scale * options->tolerance;
    vtOptions.lineMetrics = options->lineMetrics;
    return std::shared_ptr<GeoJSONData>(new GeoJSONVTData(geoJSON, vtOptions, std::move(sequencedScheduler)));
}

GeoJSONSource::Impl::Impl(std::string id_, Immutable<GeoJSONOptions> options_)
    : Source::Impl(SourceType::GeoJSON, std::move(id_)),
      options(std::move(options_)) {}

GeoJSONSource::Impl::Impl(const GeoJSONSource::Impl& other, std::shared_ptr<GeoJSONData> data_)
    : Source::Impl(other),
      options(other.options),
      data(std::move(data_)) {}

GeoJSONSource::Impl::~Impl() = default;

Range<uint8_t> GeoJSONSource::Impl::getZoomRange() const {
    return {options->minzoom, options->maxzoom};
}

std::weak_ptr<GeoJSONData> GeoJSONSource::Impl::getData() const {
    return data;
}

std::optional<std::string> GeoJSONSource::Impl::getAttribution() const {
    return {};
}

bool GeoJSONSource::Impl::isUpdateSynchronous() const {
    return options->synchronousUpdate;
}

} // namespace style
} // namespace mbgl
