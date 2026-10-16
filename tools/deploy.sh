#!/bin/bash
# Deploy web/dist (built by tools/build-release.sh) to Cloudflare Pages.
#   npx wrangler login        (once, in a browser)
#   tools/deploy.sh           (PROJECT=<name> to use another Pages project; default shrek2-team-action)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="${PROJECT:-shrek2-team-action}"
cd "$ROOT"
[ -f web/dist/index.html ] || { echo "build first: REPO_URL=... tools/build-release.sh"; exit 1; }
# Wrangler 4.15x delegates new Pages projects to Cloudflare Workers; --force creates a classic
# Pages project (served at <project>.pages.dev with the _headers file), needed only once.
if ! npx wrangler pages project list 2>/dev/null | grep -q "\b${PROJECT}\b"; then
  npx wrangler pages project create "$PROJECT" --production-branch main --force
fi
npx wrangler pages deploy web/dist --project-name "$PROJECT" --branch main --commit-dirty=true
