#!/bin/bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
models="$(cd "$here/../.." && pwd)/models"
build="$here/build"
mkdir -p "$build"
(cd "$here/../server" && cargo build --release)
out="$here/../../target/release"
clang++ -c -O2 -std=c++17 -I"$here" -fobjc-arc -DSELFTEST_MODEL="\"$models/Haru/Haru.model3.json\"" "$here/selftest.mm" -o "$build/selftest.o"
clang++ "$build/selftest.o" "$out/libhikari.a" \
    -framework Metal -framework Foundation -framework ImageIO -framework CoreGraphics -framework AppKit -framework QuartzCore -framework IOSurface -lc++ -o "$build/engine_selftest"
