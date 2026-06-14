# Learnings

## Metal shadow frustum depth mapping

- What we learned: `matrix::ortho()` uses GL-style near/far depth semantics and is not safe for projecting an arbitrary light-space AABB directly into a Metal shadow map.
- Why it matters: passing `box.min.z`/`box.max.z` as near/far can put caster geometry outside Metal's `[0, w]` clip range, leaving the shadow target white while the receiver still compares invalid depths.
- How to apply it: in `src/mbgl/renderer/shadows/shadow_frustum.cpp`, map the light-space AABB directly to Metal clip space: X/Y to `[-1, 1]`, Z to `[0, 1]`. Verify with forced caster outputs:
  `cmake --build build-macos-metal --target mbgl-render -j8` and the warm `mbgl-render` shadow command from the task.
