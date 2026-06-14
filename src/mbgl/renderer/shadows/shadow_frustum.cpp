#include <mbgl/renderer/shadows/shadow_frustum.hpp>

#include <mbgl/util/mat4.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace mbgl {

namespace {
vec3 normalize(const vec3& v) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const double i = l > 0 ? 1.0 / l : 0.0;
    return {v[0] * i, v[1] * i, v[2] * i};
}
vec3 cross(const vec3& a, const vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
} // namespace

mat4 ShadowFrustum::lightView(const vec3& sunDir) {
    // Camera at the sun direction, looking back toward the origin.
    const vec3 fwd = normalize({-sunDir[0], -sunDir[1], -sunDir[2]});
    const vec3 worldUp = (std::abs(fwd[2]) > 0.99) ? vec3{0, 1, 0} : vec3{0, 0, 1};
    const vec3 right = normalize(cross(worldUp, fwd));
    const vec3 up = cross(fwd, right);

    // Column-major (glmatrix) rotation-only view; translation folded into the ortho fit.
    mat4 m;
    matrix::identity(m);
    m[0] = right[0]; m[4] = right[1]; m[8] = right[2];
    m[1] = up[0];    m[5] = up[1];    m[9] = up[2];
    m[2] = fwd[0];   m[6] = fwd[1];   m[10] = fwd[2];
    return m;
}

ShadowFrustum::Aabb ShadowFrustum::lightSpaceAabb(const vec3& sunDir, const std::vector<vec3>& worldPoints) {
    const mat4 view = lightView(sunDir);
    Aabb box{{1e30, 1e30, 1e30}, {-1e30, -1e30, -1e30}};
    for (const auto& p : worldPoints) {
        vec4 lp;
        matrix::transformMat4(lp, vec4{{p[0], p[1], p[2], 1.0}}, view);
        for (int i = 0; i < 3; ++i) {
            box.min[i] = std::min(box.min[i], lp[i]);
            box.max[i] = std::max(box.max[i], lp[i]);
        }
    }
    return box;
}

double ShadowFrustum::texelSnap(double value, double texelWorldSize) {
    if (texelWorldSize <= 0) return value;
    return std::floor(value / texelWorldSize) * texelWorldSize;
}

std::vector<vec3> ShadowFrustum::heightExpand(const std::vector<vec3>& groundPoints, double maxHeight) {
    std::vector<vec3> out;
    out.reserve(groundPoints.size() * 2);
    for (const auto& p : groundPoints) {
        out.push_back({p[0], p[1], 0.0});
        out.push_back({p[0], p[1], maxHeight});
    }
    return out;
}

mat4 ShadowFrustum::fit(const vec3& sunDir,
                        const std::vector<vec3>& worldPoints,
                        uint32_t mapSize,
                        bool texelSnapEnabled) {
    const mat4 view = lightView(sunDir);
    Aabb box = lightSpaceAabb(sunDir, worldPoints);

    if (texelSnapEnabled && mapSize > 0) {
        const double texelX = (box.max[0] - box.min[0]) / static_cast<double>(mapSize);
        const double texelY = (box.max[1] - box.min[1]) / static_cast<double>(mapSize);
        box.min[0] = texelSnap(box.min[0], texelX);
        box.min[1] = texelSnap(box.min[1], texelY);
    }

    // Guard against a degenerate (zero-extent) range producing a singular projection.
    for (int i = 0; i < 3; ++i) {
        if (box.max[i] - box.min[i] < 1e-6) {
            box.max[i] = box.min[i] + 1.0;
        }
    }

    // Small depth margin so casters exactly on the fitted near/far plane aren't clipped,
    // without over-expanding the range (which would crush the usable depth precision).
    {
        const double zPad = (box.max[2] - box.min[2]) * 0.02 + 1.0;
        box.min[2] -= zPad;
        box.max[2] += zPad;
    }

    const double invX = 1.0 / (box.max[0] - box.min[0]);
    const double invY = 1.0 / (box.max[1] - box.min[1]);
    const double invZ = 1.0 / (box.max[2] - box.min[2]);

    mat4 ortho;
    matrix::identity(ortho);
    ortho[0] = 2.0 * invX;
    ortho[5] = 2.0 * invY;
    ortho[10] = invZ;
    ortho[12] = -(box.max[0] + box.min[0]) * invX;
    ortho[13] = -(box.max[1] + box.min[1]) * invY;
    ortho[14] = -box.min[2] * invZ;

    mat4 out;
    matrix::multiply(out, ortho, view);
    return out;
}

} // namespace mbgl
