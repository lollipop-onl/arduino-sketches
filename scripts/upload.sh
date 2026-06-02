#!/usr/bin/env bash
# コンパイルしてボードに書き込む。
set -euo pipefail

sketch="${1:?usage: upload.sh <sketch>}"
dir="sketches/${sketch}"
[ -d "$dir" ] || { echo "error: sketch not found: $dir" >&2; exit 1; }

here="$(cd "$(dirname "$0")" && pwd)"
"$here/gen-secrets.sh" "$sketch"

arduino-cli compile --fqbn "$ARDUINO_FQBN" --upload --port "$ARDUINO_PORT" "$dir"
