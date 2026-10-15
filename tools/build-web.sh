#!/bin/bash
# Build the web runtimes and stage the deployable player engine for the shell.
#   tools/build-web.sh          (run from anywhere)
# - work/build-web/shrek2_player.{js,wasm}: runtime without game code -> web/public/engine/
# - work/build-web/shrek2.{js,wasm}: development engine with C-backend game code (stays in work/)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/work/tools/emsdk/emsdk_env.sh" > /dev/null 2>&1
[ -f "$ROOT/work/tools/ffmpeg-web/lib/libavcodec.a" ] || "$ROOT/tools/build-ffmpeg-web.sh"
emcmake cmake -S "$ROOT/runtime" -B "$ROOT/work/build-web" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEN_OPT=-O2 > /dev/null
ninja -C "$ROOT/work/build-web" shrek2_player ${WITH_DEV_ENGINE:+shrek2}
mkdir -p "$ROOT/web/public/engine"
cp "$ROOT/work/build-web/shrek2_player.js" "$ROOT/work/build-web/shrek2_player.wasm" "$ROOT/web/public/engine/"
echo "player engine staged in web/public/engine"
