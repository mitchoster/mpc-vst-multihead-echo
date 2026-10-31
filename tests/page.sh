#!/usr/bin/env bash
# The companion page's script, executed without a browser (vst/web/pagetest.mjs).
# Catches the class of bug no compiler sees: a name clash, a control that registers no hit
# region, a knob that posts nothing. Needs node; without it the check is skipped loudly.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v node >/dev/null; then
  echo "SKIP: no node — the page logic test did not run (install nodejs to close this gate)"
  exit 0
fi
node "$HERE/web/pagetest.mjs" "$HERE/web/index.html"
