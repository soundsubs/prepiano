#!/usr/bin/env bash
# Build + render PrePiano off-hardware for sound-design iteration.
#   ./scripts/build_desktop.sh   ->   build/prepiano_demo.wav
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O3 -Wall -Wextra -Isrc}"
mkdir -p build

echo "==> compiling desktop demo"
$CC $CFLAGS -o build/prepiano_demo test/render_demo.c src/prepiano_dsp.c -lm

echo "==> rendering demo tour"
./build/prepiano_demo build/prepiano_demo.wav

echo "done: build/prepiano_demo.wav"
