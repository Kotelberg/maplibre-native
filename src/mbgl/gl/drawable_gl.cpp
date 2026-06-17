#include <mbgl/gl/drawable_gl.hpp>
#include <mbgl/gl/drawable_gl_impl.hpp>
#include <mbgl/gl/texture2d.hpp>
#include <mbgl/gl/upload_pass.hpp>
#include <mbgl/gl/vertex_array.hpp>
#include <mbgl/gl/vertex_attribute_gl.hpp>
#include <mbgl/gl/vertex_buffer_resource.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/shaders/gl/shader_program_gl.hpp>
#include <mbgl/util/instrumentation.hpp>
#include <mbgl/util/logging.hpp>

namespace mbgl {
namespace gl {

DrawableGL::DrawableGL(std::string name_)
    : Drawable(std::move(name_)),
      impl(std::make_unique<Impl>()) {}

DrawableGL::~DrawableGL() {
    impl->attributeBuffers.clear();
}

void DrawableGL::draw(PaintParameters& parameters) const {
    MLN_TRACE_FUNC();

    if (isCustom) {
        return;
    }

    auto& context = static_cast<gl::Context&>(parameters.context);

    if (shader) {
        const auto& shaderGL = static_cast<const ShaderProgramGL&>(*shader);
        if (shaderGL.getGLProgramID() != context.program.getCurrentValue()) {
            context.program = shaderGL.getGLProgramID();
        }
    }
    if (!shader || context.program.getCurrentValue() == 0) {
        mbgl::Log::Warning(Event::General, "Missing shader for drawable " + util::toString(getID()) + "/" + getName());
        assert(false);
        return;
    }

    if (enableDepth) {
        context.setDepthMode(getIs3D() ? parameters.depthModeFor3D()
                                       : parameters.depthModeForSublayer(getSubLayerIndex(), getDepthType()));
    } else {
        context.setDepthMode(gfx::DepthMode::disabled());
    }

    // force disable depth test for debugging
    // context.setDepthMode({gfx::DepthFunctionType::Always, gfx::DepthMaskType::ReadOnly, {0,1}});

    // For 3D mode, stenciling is handled by the layer group
    if (!is3D) {
        context.setStencilMode(makeStencilMode(parameters));
    }

    context.setColorMode(getColorMode());
    context.setCullFaceMode(getCullFaceMode());

    context.setScissorTest(parameters.scissorRect);

    impl->uniformBuffers.bind();
    bindTextures();

    for (const auto& seg : impl->segments) {
        const auto& glSeg = static_cast<DrawSegmentGL&>(*seg);
        const auto& mlSeg = glSeg.getSegment();
        if (mlSeg.indexLength > 0 && glSeg.getVertexArray().isValid()) {
            context.bindVertexArray = glSeg.getVertexArray().getID();
            if (impl->instanceCount > 0) {
                context.drawInstanced(glSeg.getMode(), mlSeg.indexOffset, mlSeg.indexLength, impl->instanceCount);
            } else {
                context.draw(glSeg.getMode(), mlSeg.indexOffset, mlSeg.indexLength);
            }
        }
    }
    // Unbind the VAO so that future buffer commands outside Drawable do not change the current VAO state
    context.bindVertexArray = value::BindVertexArray::Default;

    unbindTextures();
    impl->uniformBuffers.unbind();
}

void DrawableGL::setIndexData(gfx::IndexVectorBasePtr indexes, std::vector<UniqueDrawSegment> segments) {
    impl->indexes = std::move(indexes);
    impl->segments = std::move(segments);
}

void DrawableGL::updateVertexAttributes(gfx::VertexAttributeArrayPtr vertices,
                                        std::size_t vertexCount,
                                        gfx::DrawMode mode,
                                        gfx::IndexVectorBasePtr indexes,
                                        const SegmentBase* segments,
                                        std::size_t segmentCount) {
    gfx::Drawable::setVertexAttributes(std::move(vertices));
    impl->vertexCount = vertexCount;

    std::vector<std::unique_ptr<Drawable::DrawSegment>> drawSegs;
    drawSegs.reserve(segmentCount);
    for (std::size_t i = 0; i < segmentCount; ++i) {
        const auto& seg = segments[i];
        auto segCopy = SegmentBase{
            // no copy constructor
            seg.vertexOffset,
            seg.indexOffset,
            seg.vertexLength,
            seg.indexLength,
            seg.sortKey,
        };
        auto drawSeg = std::make_unique<DrawableGL::DrawSegmentGL>(
            mode, std::move(segCopy), VertexArray{{nullptr, false}});
        drawSegs.push_back(std::move(drawSeg));
    }

    impl->indexes = std::move(indexes);
    impl->segments = std::move(drawSegs);
}

void DrawableGL::setVertices(std::vector<uint8_t>&& data, std::size_t count, gfx::AttributeDataType type_) {
    impl->vertexData = std::move(data);
    impl->vertexCount = count;
    impl->vertexType = type_;
}

const gfx::UniformBufferArray& DrawableGL::getUniformBuffers() const {
    return impl->uniformBuffers;
}

gfx::UniformBufferArray& DrawableGL::mutableUniformBuffers() {
    return impl->uniformBuffers;
}

void DrawableGL::setVertexAttrId(const size_t id) {
    impl->vertexAttrId = id;
}

struct IndexBufferGL : public gfx::IndexBufferBase {
    IndexBufferGL(std::unique_ptr<gfx::IndexBuffer>&& buffer_)
        : buffer(std::move(buffer_)) {}
    ~IndexBufferGL() override = default;

    std::unique_ptr<mbgl::gfx::IndexBuffer> buffer;
};

void DrawableGL::upload(gfx::UploadPass& uploadPass) {
    if (isCustom) {
        return;
    }
    if (!shader) {
        Log::Warning(Event::General, "Missing shader for drawable " + util::toString(getID()) + "/" + getName());
        assert(false);
        return;
    }

    MLN_TRACE_FUNC();
#ifdef MLN_TRACY_ENABLE
    {
        auto str = name + "/" + (tileID ? util::toString(*tileID) : std::string());
        MLN_ZONE_STR(str);
    }
#endif

    auto& context = uploadPass.getContext();
    auto& glContext = static_cast<gl::Context&>(context);
    constexpr auto usage = gfx::BufferUsageType::StaticDraw;

    // Create an index buffer if necessary}
    if (impl->indexes && (!impl->indexes->getBuffer() || impl->indexes->getDirty())) {
        MLN_TRACE_ZONE(build indexes);
        auto indexBufferResource{
            uploadPass.createIndexBufferResource(impl->indexes->data(), impl->indexes->bytes(), usage)};
        auto indexBuffer = std::make_unique<gfx::IndexBuffer>(impl->indexes->elements(),
                                                              std::move(indexBufferResource));
        auto buffer = std::make_unique<IndexBufferGL>(std::move(indexBuffer));
        impl->indexes->setBuffer(std::move(buffer));
        impl->indexes->setDirty(false);
    }

    // Track whether any binding array was (re)built this upload so the VAOs get rebuilt below.
    bool bindingsRebuilt = false;

    // Build the vertex attributes and bindings, if necessary
    if (impl->attributeBindings.empty() ||
        (vertexAttributes && (!attributeUpdateTime || vertexAttributes->isModifiedAfter(*attributeUpdateTime)))) {
        MLN_TRACE_ZONE(build attributes);

        // Apply drawable values to shader defaults
        const auto& defaults = shader->getVertexAttributes();
        const auto& overrides = *vertexAttributes;

        const auto& indexAttribute = defaults.get(impl->vertexAttrId);
        const auto vertexAttributeIndex = static_cast<std::size_t>(indexAttribute ? indexAttribute->getIndex() : -1);

        std::vector<std::unique_ptr<gfx::VertexBufferResource>> vertexBuffers;
        impl->attributeBindings = uploadPass.buildAttributeBindings(impl->vertexCount,
                                                                    impl->vertexType,
                                                                    vertexAttributeIndex,
                                                                    impl->vertexData,
                                                                    defaults,
                                                                    overrides,
                                                                    usage,
                                                                    attributeUpdateTime,
                                                                    vertexBuffers);

        impl->attributeBuffers = std::move(vertexBuffers);
        bindingsRebuilt = true;
    }

    // Build per-instance attribute bindings (divisor 1), if any. Mirrors the Metal drawable's
    // instance path; the GL-specific part is that these are merged into the same VAO below and
    // flagged with instanceDivisor = 1 so VertexAttribute::Set issues glVertexAttribDivisor.
    if (instanceAttributes && (impl->instanceAttributeBindings.empty() || !attributeUpdateTime ||
                               instanceAttributes->isModifiedAfter(*attributeUpdateTime))) {
        MLN_TRACE_ZONE(build instance attributes);

        std::vector<std::unique_ptr<gfx::VertexBufferResource>> instanceBuffers;
        auto instanceBindings = uploadPass.buildAttributeBindings(instanceAttributes->getMinCount(),
                                                                  gfx::AttributeDataType::Byte,
                                                                  /*vertexAttributeIndex=*/static_cast<std::size_t>(-1),
                                                                  /*vertexData=*/{},
                                                                  shader->getInstanceAttributes(),
                                                                  *instanceAttributes,
                                                                  usage,
                                                                  attributeUpdateTime,
                                                                  instanceBuffers);

        for (auto& binding : instanceBindings) {
            if (binding) {
                binding->instanceDivisor = 1;
            }
        }

        impl->instanceAttributeBindings = std::move(instanceBindings);
        impl->instanceAttributeBuffers = std::move(instanceBuffers);
        impl->instanceCount = instanceAttributes->getMinCount();
        instanceAttributes->visitAttributes([](gfx::VertexAttribute& attrib) { attrib.setDirty(false); });
        bindingsRebuilt = true;
    }

    // If any binding array changed, invalidate the existing VAOs so they are rebuilt from the
    // merged vertex+instance bindings below (review P2: instance data set after the first upload
    // would otherwise be silently ignored by an already-valid VAO).
    if (bindingsRebuilt) {
        for (const auto& seg : impl->segments) {
            static_cast<DrawSegmentGL&>(*seg).setVertexArray(VertexArray{{nullptr, false}});
        }
    }

    // Bind a VAO for each group of vertexes described by a segment
    for (const auto& seg : impl->segments) {
        MLN_TRACE_ZONE(segment);
        auto& glSeg = static_cast<DrawSegmentGL&>(*seg);
        const auto& mlSeg = glSeg.getSegment();

        if (mlSeg.indexLength == 0) {
            continue;
        }

        // The segment vertex offset applies to the per-vertex attributes only. Instance
        // attributes (divisor 1) advance per instance, not per segment vertex, so their offset
        // stays 0 (the fill-extrusion instanced path uses a single static-quad segment anyway).
        for (auto& binding : impl->attributeBindings) {
            if (binding) {
                binding->vertexOffset = static_cast<uint32_t>(mlSeg.vertexOffset);
            }
        }

        if (!glSeg.getVertexArray().isValid() && impl->indexes) {
            auto vertexArray = glContext.createVertexArray();
            const auto& indexBuffer = static_cast<IndexBufferGL&>(*impl->indexes->getBuffer());

            if (impl->instanceAttributeBindings.empty()) {
                vertexArray.bind(glContext, *indexBuffer.buffer, impl->attributeBindings);
            } else {
                // Merge per-vertex (divisor 0) and per-instance (divisor 1) bindings into one
                // location-indexed array so both live in the same VAO. Locations never collide:
                // the shader's vertex vs instance attribute metadata assigns disjoint locations.
                AttributeBindingArray merged = impl->attributeBindings;
                for (std::size_t loc = 0; loc < impl->instanceAttributeBindings.size(); ++loc) {
                    if (impl->instanceAttributeBindings[loc]) {
                        if (merged.size() <= loc) {
                            merged.resize(loc + 1);
                        }
                        merged[loc] = impl->instanceAttributeBindings[loc];
                    }
                }
                vertexArray.bind(glContext, *indexBuffer.buffer, merged);
            }
            assert(vertexArray.isValid());
            if (vertexArray.isValid()) {
                glSeg.setVertexArray(std::move(vertexArray));
            }
        }
    }

    const auto needsUpload = [](const auto& texture) {
        return texture && texture->needsUpload();
    };
    if (std::any_of(textures.begin(), textures.end(), needsUpload)) {
        uploadTextures();
    }

    attributeUpdateTime = util::MonotonicTimer::now();
}

gfx::ColorMode DrawableGL::makeColorMode(PaintParameters& parameters) const {
    return enableColor ? parameters.colorModeForRenderPass() : gfx::ColorMode::disabled();
}

gfx::StencilMode DrawableGL::makeStencilMode(PaintParameters& parameters) const {
    if (enableStencil) {
        if (!is3D && tileID) {
            return parameters.stencilModeForClipping(tileID->toUnwrapped());
        }
        assert(false);
    }
    return gfx::StencilMode::disabled();
}

void DrawableGL::uploadTextures() const {
    MLN_TRACE_FUNC();
    for (const auto& texture : textures) {
        if (texture) {
            texture->upload();
        }
    }
}

void DrawableGL::bindTextures() const {
    int32_t unit = 0;
    for (size_t id = 0; id < textures.size(); id++) {
        if (const auto& texture = textures[id]) {
            if (const auto& location = shader->getSamplerLocation(id)) {
                static_cast<gl::Texture2D&>(*texture).bind(static_cast<int32_t>(*location), unit++);
            }
        }
    }
}

void DrawableGL::unbindTextures() const {
    for (const auto& texture : textures) {
        if (texture) {
            static_cast<gl::Texture2D&>(*texture).unbind();
        }
    }
}

} // namespace gl
} // namespace mbgl
