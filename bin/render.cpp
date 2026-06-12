#include <mbgl/map/map.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/run_loop.hpp>

#include <mbgl/gfx/backend.hpp>
#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/style/style.hpp>

#if !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
#include <mbgl/style/layers/custom_drawable_layer.hpp>
#include <mbgl/style/layers/debug_cube_layer_host.hpp>
#endif

#include <mbgl/style/conversion/geojson.hpp>
#include <mbgl/style/expression/dsl.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/sources/geojson_source.hpp>

#include <args.hxx>

#include <cstdlib>
#include <iostream>
#include <fstream>

int main(int argc, char* argv[]) {
    args::ArgumentParser argumentParser("MapLibre Native render tool");
    args::HelpFlag helpFlag(argumentParser, "help", "Display this help menu", {"help"});

    args::ValueFlag<std::string> backendValue(argumentParser, "Backend", "Rendering backend", {"backend"});
    args::ValueFlag<std::string> apikeyValue(argumentParser, "key", "API key", {'t', "apikey"});
    args::ValueFlag<std::string> styleValue(argumentParser, "URL", "Map stylesheet", {'s', "style"});
    args::ValueFlag<std::string> outputValue(argumentParser, "file", "Output file name", {'o', "output"});
    args::ValueFlag<std::string> cacheValue(argumentParser, "file", "Cache database file name", {'c', "cache"});
    args::ValueFlag<std::string> assetsValue(
        argumentParser, "file", "Directory to which asset:// URLs will resolve", {'a', "assets"});

    args::Flag debugFlag(argumentParser, "debug", "Debug mode", {"debug"});
    args::Flag debugCubeFlag(
        argumentParser, "debug-cube", "Add the M1 debug cube layer at the camera center (placement/depth check)", {"debug-cube"});
    args::Flag modelLayerFlag(
        argumentParser, "model-layer", "Add an M3a model layer with 3 demo points around the camera center", {"model-layer"});

    args::ValueFlag<double> pixelRatioValue(argumentParser, "number", "Image scale factor", {'r', "ratio"});

    // grouping ensures either bounds or center based position is used
    args::Group boundsOrCenterZoom(argumentParser, "Position (either one):", args::Group::Validators::AtMostOne);

    args::NargsValueFlag<double> boundsValue(
        boundsOrCenterZoom, "degrees: north west south east", "Bounds of rendered map", {"bounds"}, 4);

    args::Group centerGroup(boundsOrCenterZoom, "Center:", args::Group::Validators::AtLeastOne);
    args::ValueFlag<double> zoomValue(centerGroup, "number", "Zoom level", {'z', "zoom"});
    args::ValueFlag<double> lonValue(centerGroup, "degrees", "Longitude", {'x', "lon"});
    args::ValueFlag<double> latValue(centerGroup, "degrees", "Latitude", {'y', "lat"});
    args::ValueFlag<double> altValue(argumentParser, "degrees", "Altitude", {'A', "alt"});
    args::ValueFlag<double> fovValue(argumentParser, "degrees", "FOV", {'f', "fov"});
    args::ValueFlag<double> bearingValue(argumentParser, "degrees", "Bearing", {'b', "bearing"});
    args::ValueFlag<double> pitchValue(argumentParser, "degrees", "Pitch", {'p', "pitch"});
    args::ValueFlag<double> rollValue(argumentParser, "degrees", "Roll", {'R', "roll"});
    args::ValueFlag<uint32_t> widthValue(argumentParser, "pixels", "Image width", {'w', "width"});
    args::ValueFlag<uint32_t> heightValue(argumentParser, "pixels", "Image height", {'h', "height"});

    args::ValueFlag<std::string> mapModeValue(
        argumentParser, "MapMode", "Map mode (e.g. 'static', 'tile', 'continuous')", {'m', "mode"});

    try {
        argumentParser.ParseCLI(argc, argv);
    } catch (const args::Help&) {
        std::cout << argumentParser;
        exit(0);
    } catch (const args::ParseError& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << argumentParser;
        exit(1);
    } catch (const args::ValidationError& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << argumentParser;
        exit(2);
    }

    const double lat = latValue ? args::get(latValue) : 0;
    const double lon = lonValue ? args::get(lonValue) : 0;
    const double alt = altValue ? args::get(altValue) : 0;
    const double zoom = zoomValue ? args::get(zoomValue) : 0;
    const double fov = fovValue ? args::get(fovValue) : mbgl::util::rad2deg(mbgl::util::DEFAULT_FOV);
    const double bearing = bearingValue ? args::get(bearingValue) : 0;
    const double pitch = pitchValue ? args::get(pitchValue) : 0;
    const double roll = rollValue ? args::get(rollValue) : 0;
    const double pixelRatio = pixelRatioValue ? args::get(pixelRatioValue) : 1;

    const uint32_t width = widthValue ? args::get(widthValue) : 512;
    const uint32_t height = heightValue ? args::get(heightValue) : 512;
    const std::string output = outputValue ? args::get(outputValue) : "out.png";
    const std::string cache_file = cacheValue ? args::get(cacheValue) : "cache.sqlite";
    const std::string asset_root = assetsValue ? args::get(assetsValue) : ".";

    // Try to load the apikey from the environment.
    const char* apikeyEnv = getenv("MLN_API_KEY");
    const std::string apikey = apikeyValue ? args::get(apikeyValue) : (apikeyEnv ? apikeyEnv : std::string());

    const bool debug = debugFlag ? args::get(debugFlag) : false;

    using namespace mbgl;

    auto mapTilerConfiguration = mbgl::TileServerOptions::MapTilerConfiguration();
    std::string style = styleValue ? args::get(styleValue) : mapTilerConfiguration.defaultStyles().at(0).getUrl();

    util::RunLoop loop;

    MapMode mapMode = MapMode::Static;
    if (mapModeValue) {
        const auto modeStr = args::get(mapModeValue);
        if (modeStr == "tile") {
            mapMode = MapMode::Tile;
        } else if (modeStr == "continuous") {
            mapMode = MapMode::Continuous;
        }
    }

    class RenderObserver : public MapObserver {
    public:
        std::function<void()> styleLoaded;
        void onDidFinishLoadingStyle() final {
            if (styleLoaded) styleLoaded();
        }
    };
    RenderObserver observer;

    HeadlessFrontend frontend({width, height}, static_cast<float>(pixelRatio));
    Map map(
        frontend,
        observer,
        MapOptions().withMapMode(mapMode).withSize(frontend.getSize()).withPixelRatio(static_cast<float>(pixelRatio)),
        ResourceOptions()
            .withCachePath(cache_file)
            .withAssetPath(asset_root)
            .withApiKey(apikey)
            .withTileServerOptions(mapTilerConfiguration));

    if (style.find("://") == std::string::npos) {
        style = std::string("file://") + style;
    }

#if !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
    if (debugCubeFlag) {
        observer.styleLoaded = [&map, lat, lon] {
            if (!map.getStyle().getLayer("debug-cube")) {
                map.getStyle().addLayer(std::make_unique<style::CustomDrawableLayer>(
                    "debug-cube", std::make_unique<style::DebugCubeLayerHost>(LatLng{lat, lon}, 50.0)));
            }
        };
    }
#endif

    if (modelLayerFlag) {
        observer.styleLoaded = [&map, lat, lon] {
            if (map.getStyle().getLayer("m3-models")) {
                return;
            }

            char geojson[1024];
            std::snprintf(geojson,
                          sizeof(geojson),
                          R"({"type":"FeatureCollection","features":[
{"type":"Feature","properties":{"bearing":0,"size":15},"geometry":{"type":"Point","coordinates":[%.6f,%.6f]}},
{"type":"Feature","properties":{"bearing":45,"size":30},"geometry":{"type":"Point","coordinates":[%.6f,%.6f]}},
{"type":"Feature","properties":{"bearing":120,"size":50},"geometry":{"type":"Point","coordinates":[%.6f,%.6f]}}]})",
                          lon - 0.0010,
                          lat + 0.0004,
                          lon,
                          lat - 0.0006,
                          lon + 0.0012,
                          lat + 0.0008);

            style::conversion::Error geojsonError;
            auto converted = style::conversion::parseGeoJSON(geojson, geojsonError);
            if (!converted) {
                std::cerr << "model-layer geojson error: " << geojsonError.message << std::endl;
                return;
            }

            auto source = std::make_unique<style::GeoJSONSource>("m3-points");
            source->setGeoJSON(*converted);
            map.getStyle().addSource(std::move(source));

            namespace dsl = style::expression::dsl;
            auto layer = std::make_unique<style::ModelLayer>("m3-models", "m3-points");
            layer->setModelRotation(style::PropertyExpression<float>(dsl::number(dsl::get("bearing"))));
            layer->setModelScale(style::PropertyExpression<float>(dsl::number(dsl::get("size"))));
            layer->setModelOpacity(0.9f);
            map.getStyle().addLayer(std::move(layer));
        };
    }

    map.getStyle().loadURL(style);
    std::vector<double> bounds = args::get(boundsValue);
    if (bounds.size() == 4) {
        LatLngBounds boundingBox = LatLngBounds::hull(LatLng(bounds[0], bounds[1]), LatLng(bounds[2], bounds[3]));
        map.jumpTo(map.cameraForLatLngBounds(boundingBox, EdgeInsets(), bearing, pitch));
    } else {
        map.jumpTo(CameraOptions()
                       .withCenter(LatLng{lat, lon})
                       .withCenterAltitude(alt)
                       .withZoom(zoom)
                       .withBearing(bearing)
                       .withPitch(pitch)
                       .withRoll(roll)
                       .withFov(fov));
    }

    if (debug) {
        map.setDebug(debug ? mbgl::MapDebugOptions::TileBorders | mbgl::MapDebugOptions::ParseStatus
                           : mbgl::MapDebugOptions::NoDebug);
    }

    try {
        std::ofstream out(output, std::ios::binary);
        out << encodePNG(frontend.render(map).image);
        out.close();
    } catch (std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        exit(1);
    }

    return 0;
}
