#!/bin/bash
# Build the minimal FFmpeg (libavcodec Bink video + Bink audio decoders, libavutil) for the web
# runtime with Emscripten. Output: work/tools/ffmpeg-web/{include,lib}.
#   tools/build-ffmpeg-web.sh [path-to-ffmpeg-source]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/work/tools/ffmpeg-dl/ffmpeg-7.1.1}"
OUT="$ROOT/work/tools/ffmpeg-web"
BUILD="$ROOT/work/tools/ffmpeg-web-build"
source "$ROOT/work/tools/emsdk/emsdk_env.sh" > /dev/null 2>&1
mkdir -p "$BUILD"
cd "$BUILD"
emconfigure "$SRC/configure" \
  --prefix="$OUT" --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm \
  --target-os=none --arch=x86_32 --enable-cross-compile \
  --disable-x86asm --disable-inline-asm --disable-asm \
  --disable-programs --disable-doc --disable-debug --disable-autodetect \
  --disable-everything --disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
  --disable-avdevice --disable-avformat --disable-swresample --disable-swscale --disable-avfilter \
  --enable-decoder=bink --enable-decoder=binkaudio_rdft --enable-decoder=binkaudio_dct \
  --enable-static --disable-shared \
  --extra-cflags="-O3 -pthread" --extra-ldflags="-pthread"
emmake make -j"$(nproc)"
emmake make install
echo "FFmpeg for the web installed in $OUT"
