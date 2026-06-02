#!/usr/bin/env bash
# スケッチをコンパイルする。事前に arduino_secrets.h を生成する。
set -euo pipefail

sketch="${1:?usage: compile.sh <sketch>}"
dir="sketches/${sketch}"
[ -d "$dir" ] || { echo "error: sketch not found: $dir" >&2; exit 1; }

here="$(cd "$(dirname "$0")" && pwd)"
"$here/gen-secrets.sh" "$sketch"

arduino-cli compile --fqbn "$ARDUINO_FQBN" "$dir"
