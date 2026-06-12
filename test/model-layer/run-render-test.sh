#!/usr/bin/env bash
# Model-layer render smoke test (fork extension).
#
# Renders the --model-layer demo (three placeholder cubes with data-driven
# bearing/size, minzoom 15) over a local background-only style — fully
# offline and deterministic — and compares against the checked-in golden.
#
# Usage: test/model-layer/run-render-test.sh [build-dir]
# Regenerate the golden after intentional changes:
#   cp /tmp/model-layer-render-test.png test/model-layer/expected.png
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${1:-$HERE/../../build-macos-metal}"
OUT=/tmp/model-layer-render-test.png
MAX_DIFF_PX=500

"$BUILD/bin/mbgl-render" \
    --style "file://$HERE/style.json" \
    -y 50.4501 -x 30.5234 -z 15.5 -p 45 -b 20 -w 512 -h 512 \
    -c /tmp/model-layer-test-cache.sqlite \
    --model-layer \
    -o "$OUT"

AE=$(magick compare -metric AE "$HERE/expected.png" "$OUT" null: 2>&1 || true)
echo "model-layer render test: ${AE} differing pixels (max ${MAX_DIFF_PX})"
if ! [ "$AE" -le "$MAX_DIFF_PX" ] 2>/dev/null; then
    echo "FAIL: render diverged from golden ($HERE/expected.png vs $OUT)"
    exit 1
fi
echo "PASS"
