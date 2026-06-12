#include <mbgl/renderer/model/glb_mesh_loader.hpp>

#include <mbgl/util/logging.hpp>

// Symbol-renamed: Filament's gltfio_core bundles its own cgltf implementation
// (and cgltf.h forces extern "C", so a namespace doesn't help). Rename every
// public cgltf function via macros before including the implementation.
#define cgltf_accessor_index mbgl_cgltf_accessor_index
#define cgltf_accessor_read_float mbgl_cgltf_accessor_read_float
#define cgltf_accessor_read_index mbgl_cgltf_accessor_read_index
#define cgltf_accessor_read_uint mbgl_cgltf_accessor_read_uint
#define cgltf_accessor_unpack_floats mbgl_cgltf_accessor_unpack_floats
#define cgltf_accessor_unpack_indices mbgl_cgltf_accessor_unpack_indices
#define cgltf_animation_channel_index mbgl_cgltf_animation_channel_index
#define cgltf_animation_index mbgl_cgltf_animation_index
#define cgltf_animation_sampler_index mbgl_cgltf_animation_sampler_index
#define cgltf_buffer_index mbgl_cgltf_buffer_index
#define cgltf_buffer_view_data mbgl_cgltf_buffer_view_data
#define cgltf_buffer_view_index mbgl_cgltf_buffer_view_index
#define cgltf_calc_size mbgl_cgltf_calc_size
#define cgltf_camera_index mbgl_cgltf_camera_index
#define cgltf_component_size mbgl_cgltf_component_size
#define cgltf_copy_extras_json mbgl_cgltf_copy_extras_json
#define cgltf_decode_string mbgl_cgltf_decode_string
#define cgltf_decode_uri mbgl_cgltf_decode_uri
#define cgltf_find_accessor mbgl_cgltf_find_accessor
#define cgltf_free mbgl_cgltf_free
#define cgltf_image_index mbgl_cgltf_image_index
#define cgltf_light_index mbgl_cgltf_light_index
#define cgltf_load_buffer_base64 mbgl_cgltf_load_buffer_base64
#define cgltf_load_buffers mbgl_cgltf_load_buffers
#define cgltf_material_index mbgl_cgltf_material_index
#define cgltf_mesh_index mbgl_cgltf_mesh_index
#define cgltf_node_index mbgl_cgltf_node_index
#define cgltf_node_transform_local mbgl_cgltf_node_transform_local
#define cgltf_node_transform_world mbgl_cgltf_node_transform_world
#define cgltf_num_components mbgl_cgltf_num_components
#define cgltf_parse mbgl_cgltf_parse
#define cgltf_parse_file mbgl_cgltf_parse_file
#define cgltf_sampler_index mbgl_cgltf_sampler_index
#define cgltf_scene_index mbgl_cgltf_scene_index
#define cgltf_skin_index mbgl_cgltf_skin_index
#define cgltf_texture_index mbgl_cgltf_texture_index
#define cgltf_validate mbgl_cgltf_validate

#define CGLTF_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <mbgl/renderer/model/cgltf.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace mbgl {
namespace model {

namespace {

using Vertex = style::CustomDrawableLayerHost::Interface::GeometryVertex;

struct RawTri {
    std::array<float, 3> p[3];
    std::array<float, 2> t[3];
};

void transformPoint(const cgltf_float m[16], const cgltf_float* in, std::array<float, 3>& out) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}

// Decode a texture image embedded in the GLB buffer (PNG/JPEG bytes).
std::shared_ptr<PremultipliedImage> decodeTexture(const cgltf_texture* texture) {
    if (!texture || !texture->image) return nullptr;
    const cgltf_image* img = texture->image;
    if (!img->buffer_view || !img->buffer_view->buffer || !img->buffer_view->buffer->data) return nullptr;
    const auto* bytes = static_cast<const uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
    try {
        return std::make_shared<PremultipliedImage>(
            decodeImage({reinterpret_cast<const char*>(bytes), img->buffer_view->size}));
    } catch (...) {
        return nullptr;
    }
}

} // namespace

BakedModel loadGlbMesh(const std::string& path) {
    BakedModel model;

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        Log::Warning(Event::General, "glb_mesh_loader: cannot parse " + path);
        return model;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        Log::Warning(Event::General, "glb_mesh_loader: cannot load buffers for " + path);
        cgltf_free(data);
        return model;
    }

    // Gather triangles per (material, baked-shade bucket): a fixed sun and
    // per-face lambert factor baked into the part color gives the flat-shaded
    // look of the source renders instead of fully-unlit flatness.
    std::map<std::pair<const cgltf_material*, int>, std::vector<RawTri>> byMaterial;
    constexpr float kSun[3] = {-0.42f, 0.33f, 0.84f}; // normalized-ish, z-up
    constexpr float kAmbient = 0.62f;
    float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
    float minY = minX, maxY = maxX, minZ = minX, maxZ = maxX;

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node* node = &data->nodes[n];
        if (!node->mesh) continue;
        cgltf_float world[16];
        cgltf_node_transform_world(node, world);

        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive* prim = &node->mesh->primitives[p];
            if (prim->type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* uv = nullptr;
            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                if (prim->attributes[a].type == cgltf_attribute_type_position) pos = prim->attributes[a].data;
                if (prim->attributes[a].type == cgltf_attribute_type_texcoord && !uv)
                    uv = prim->attributes[a].data;
            }
            if (!pos) continue;

            const cgltf_size count = prim->indices ? prim->indices->count : pos->count;

            for (cgltf_size i = 0; i + 2 < count; i += 3) {
                RawTri tri{};
                for (int k = 0; k < 3; ++k) {
                    const cgltf_size idx = prim->indices ? cgltf_accessor_read_index(prim->indices, i + k) : i + k;
                    cgltf_float v[3] = {0, 0, 0};
                    cgltf_accessor_read_float(pos, idx, v, 3);
                    std::array<float, 3> world_p{};
                    transformPoint(world, v, world_p);
                    // glTF +Y-up → map +Z-up: (x, y, z) → (x, -z, y)
                    // (verified against the asset's reference render: roof up)
                    tri.p[k] = {world_p[0], -world_p[2], world_p[1]};
                    if (uv) {
                        cgltf_float t[2] = {0, 0};
                        cgltf_accessor_read_float(uv, idx, t, 2);
                        tri.t[k] = {t[0], t[1]};
                    }
                    minX = std::min(minX, tri.p[k][0]);
                    maxX = std::max(maxX, tri.p[k][0]);
                    minY = std::min(minY, tri.p[k][1]);
                    maxY = std::max(maxY, tri.p[k][1]);
                    minZ = std::min(minZ, tri.p[k][2]);
                    maxZ = std::max(maxZ, tri.p[k][2]);
                }
                // Face normal in the map frame → lambert shade bucket
                const float ux = tri.p[1][0] - tri.p[0][0], uy = tri.p[1][1] - tri.p[0][1],
                            uz = tri.p[1][2] - tri.p[0][2];
                const float vx = tri.p[2][0] - tri.p[0][0], vy = tri.p[2][1] - tri.p[0][1],
                            vz = tri.p[2][2] - tri.p[0][2];
                float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                float shade = 1.0f;
                if (len > 0) {
                    const float d = (nx * kSun[0] + ny * kSun[1] + nz * kSun[2]) / len;
                    shade = kAmbient + (1.0f - kAmbient) * std::max(0.0f, d);
                }
                const int bucketKey = static_cast<int>(std::lround(shade * 16.0f));
                byMaterial[{prim->material, bucketKey}].push_back(tri);
            }
        }
    }

    const float height = maxZ - minZ;
    if (byMaterial.empty() || height <= 0) {
        Log::Warning(Event::General, "glb_mesh_loader: no triangles in " + path);
        cgltf_free(data);
        return model;
    }
    const float cx = (minX + maxX) * 0.5f;
    const float cy = (minY + maxY) * 0.5f;
    const float invH = 1.0f / height;

    // Emit parts: per material, split into <=64k-vertex chunks (uint16 indices).
    constexpr size_t kMaxVerts = 65532;
    for (auto& [key, tris] : byMaterial) {
        const cgltf_material* material = key.first;
        const float shade = static_cast<float>(key.second) / 16.0f;
        std::shared_ptr<PremultipliedImage> texture;
        Color color = Color::white();
        if (material) {
            if (material->has_pbr_metallic_roughness) {
                texture = decodeTexture(material->pbr_metallic_roughness.base_color_texture.texture);
                // glTF semantics: baseColor = texture * factor (factor alone when untextured)
                const auto* c = material->pbr_metallic_roughness.base_color_factor;
                color = Color{c[0], c[1], c[2], c[3]};
            }
        }

        size_t i = 0;
        while (i < tris.size()) {
            BakedModel::Part part;
            part.vertices = std::make_shared<gfx::VertexVector<Vertex>>();
            part.indices = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
            part.texture = texture;
            part.color = Color{color.r * shade, color.g * shade, color.b * shade, color.a};

            while (i < tris.size() && part.vertices->elements() + 3 <= kMaxVerts) {
                const RawTri& tri = tris[i++];
                const auto base = static_cast<uint16_t>(part.vertices->elements());
                for (int k = 0; k < 3; ++k) {
                    part.vertices->emplace_back(
                        Vertex{.position = {(tri.p[k][0] - cx) * invH, (tri.p[k][1] - cy) * invH,
                                            (tri.p[k][2] - minZ) * invH},
                               .texcoords = tri.t[k]});
                }
                part.indices->emplace_back(base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2));
            }
            model.parts.push_back(std::move(part));
        }
    }

    cgltf_free(data);
    model.valid = true;
    return model;
}

} // namespace model
} // namespace mbgl
