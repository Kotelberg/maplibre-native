#if MLN_RENDER_BACKEND_OPENGL

#include <mbgl/test/util.hpp>
#include <mbgl/gfx/attribute.hpp>

using namespace mbgl;

TEST(GLInstancing, AttributeBindingCarriesDivisor) {
    gfx::AttributeBinding a{};
    a.instanceDivisor = 1;
    gfx::AttributeBinding b = a;
    EXPECT_EQ(b.instanceDivisor, 1u);
    EXPECT_TRUE(a == b);
    b.instanceDivisor = 0;
    EXPECT_FALSE(a == b); // divisor participates in equality (VAO cache key)
}

#endif // MLN_RENDER_BACKEND_OPENGL
