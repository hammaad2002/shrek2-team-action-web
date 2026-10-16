#!/bin/bash
# Build the deployable site into web/dist and run the release guards.
#   REPO_URL=https://github.com/<owner>/<repo> tools/build-release.sh
# Guards (any failure stops the release):
#   - no development-only references in the build (dev engine, dev disc image, work/, research/)
#   - tools/scan-dist.mjs: no byte run shared with the game's files (installed tree and disc),
#     except third-party tables also present in FFmpeg / iced-x86 as shipped
#   - universal-modder `um publish check` on web/dist and on a clean export of the repository
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${REPO_URL:?set REPO_URL to the public source repository (shown on the page and the licenses page)}"
UM="${UM:-$HOME/repos/universal-modder/bin/um}"
DIST="$ROOT/web/dist"

"$ROOT/tools/build-web.sh"
(cd "$ROOT/web" && npx vite build)
sed -i "s#__REPO_URL__#${REPO_URL}#g" "$DIST/index.html" "$DIST/licenses.html"

echo "== placeholders and development references"
if grep -rl "__REPO_URL__" "$DIST"; then echo "FAIL: unfilled placeholder"; exit 1; fi
if grep -rlE "/dev-engine|/dev-iso|work/(install|build|iso|tools)|research/" "$DIST"; then echo "FAIL: development reference"; exit 1; fi
echo "ok"

echo "== shared bytes with the game"
node "$ROOT/tools/scan-dist.mjs" "$DIST" "$ROOT/work/install" "$ROOT/work/iso" \
  --allow "$ROOT/work/tools/ffmpeg-web/lib/libavcodec.a" --allow "$ROOT/node_modules/iced-x86/iced_x86_bg.wasm" \
  --allow "$ROOT/tools/scan-allow/mit-license.txt"

echo "== um publish check (site)"
"$UM" publish check "$DIST" --game "$ROOT/work/install/Game"

echo "== um publish check (repository, committed files only)"
EXPORT="$(mktemp -d)"
trap 'rm -rf "$EXPORT"' EXIT
git -C "$ROOT" archive HEAD | tar -x -C "$EXPORT"
"$UM" publish check "$EXPORT" --game "$ROOT/work/install/Game"

echo "release build ready in $DIST"
