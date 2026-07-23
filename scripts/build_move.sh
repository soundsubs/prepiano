#!/usr/bin/env bash
# Cross-compile the PrePiano DSP plugin for Ableton Move (ARM64), assemble the
# module package, and optionally deploy it to the device.
#
# Mirrors the noizboy/drmach flow: cross-compile dsp.so -> stage module dir ->
# tar.gz -> scp to Move -> rescan.
#
# Config via env:
#   CROSS_PREFIX   ARM64 toolchain prefix   (default: aarch64-linux-gnu-)
#   SCHWUNG_SRC    path to a checkout of charlesvestal/schwung `src/`, which
#                  provides the authoritative host/plugin_api_v1.h. If unset,
#                  the bundled reference stub in src/host/ is used.
#   MOVE_HOST      device hostname          (default: move.local)
#   MOVE_USER      device ssh user          (default: ableton)
#   DEPLOY=1       scp + install to the device after building
#
# Examples:
#   ./scripts/build_move.sh
#   SCHWUNG_SRC=../schwung/src DEPLOY=1 ./scripts/build_move.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
CC="${CROSS_PREFIX}gcc"
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"

MOD=prepiano
OUT="build/modules/${MOD}"
mkdir -p "$OUT"

# Include paths: our src first, then the real Schwung headers if provided.
INCS="-Isrc"
if [[ -n "${SCHWUNG_SRC:-}" ]]; then
  # put real headers FIRST so the authoritative host/plugin_api_v1.h wins
  INCS="-I${SCHWUNG_SRC} -Isrc"
  echo "==> using Schwung headers from ${SCHWUNG_SRC}"
else
  echo "==> SCHWUNG_SRC not set: building against bundled reference stub (src/host/)"
fi

if ! command -v "$CC" >/dev/null 2>&1; then
  echo "!! cross compiler '$CC' not found."
  echo "   Install an aarch64 gcc (e.g. gcc-aarch64-linux-gnu) or set CROSS_PREFIX,"
  echo "   or build inside the Schwung Docker image as noizboy does."
  exit 1
fi

echo "==> cross-compiling ${MOD} dsp.so for ARM64"
$CC -O3 -shared -fPIC $INCS \
    src/plugin/prepiano_plugin.c src/prepiano_dsp.c \
    -o "${OUT}/dsp.so" -lm

echo "==> staging module package"
cp src/modules/${MOD}/module.json "${OUT}/module.json"
# optional custom UI (PrePiano uses the stock Shadow UI, so this is usually absent)
[ -f src/modules/${MOD}/ui.js ] && cp src/modules/${MOD}/ui.js "${OUT}/ui.js" || true

TARBALL="build/${MOD}-module.tar.gz"
tar -C build/modules -czf "$TARBALL" "${MOD}"
echo "==> packaged ${TARBALL}"
ls -la "${OUT}"

if [[ "${DEPLOY:-0}" == "1" ]]; then
  echo "==> deploying to ${MOVE_USER}@${MOVE_HOST}"
  DEST="/data/UserData/schwung/modules"
  ssh "${MOVE_USER}@${MOVE_HOST}" "mkdir -p ${DEST}"
  scp -r "${OUT}" "${MOVE_USER}@${MOVE_HOST}:${DEST}/"
  echo "==> module copied. Rescan from the Module Manager (http://${MOVE_HOST}:7700)"
  echo "    or re-enter Schwung to pick up ${MOD}."
fi

echo "done."
