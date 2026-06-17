#!/usr/bin/env bash
# Fill-extrusion GL-instancing render parity test (fork extension).
#
# Renders a fixed two-building fill-extrusion style (data-driven height) at a
# pitched camera so the walls are visible — fully offline and deterministic — and
# compares against the checked-in golden. The golden is captured with the
# non-instanced GL path (MLN_GL_FE_INSTANCING=0); rebuilding mbgl-render with the
# gate ON and re-running must match it within the threshold (visual parity).
#
# Usage: test/fill-extrusion-instancing/run-render-test.sh [build-dir]
# Regenerate the golden after intentional changes (with the gate OFF):
#   cp /tmp/fill-extrusion-instancing-render-test.png test/fill-extrusion-instancing/expected.png
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${1:-$HERE/../../build-gl-check}"
OUT=/tmp/fill-extrusion-instancing-render-test.png
MAX_DIFF_PX=500

"$BUILD/bin/mbgl-render" \
    --style "file://$HERE/style.json" \
    -y 50.4501 -x 30.5234 -z 16.5 -p 55 -b 20 -w 512 -h 512 \
    -c /tmp/fill-extrusion-instancing-test-cache.sqlite \
    -o "$OUT"

if [ ! -f "$HERE/expected.png" ]; then
    echo "No golden yet — wrote $OUT. Review it, then:"
    echo "  cp $OUT $HERE/expected.png"
    exit 0
fi

AE=$(magick compare -metric AE "$HERE/expected.png" "$OUT" null: 2>&1 || true)
echo "fill-extrusion-instancing render test: ${AE} differing pixels (max ${MAX_DIFF_PX})"
if ! [ "$AE" -le "$MAX_DIFF_PX" ] 2>/dev/null; then
    echo "FAIL: render diverged from golden ($HERE/expected.png vs $OUT)"
    exit 1
fi
echo "PASS"
