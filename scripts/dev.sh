#!/usr/bin/env bash
# 書き込み後そのままシリアルモニタを開く(開発ループ用)。
set -euo pipefail

sketch="${1:?usage: dev.sh <sketch>}"
here="$(cd "$(dirname "$0")" && pwd)"

"$here/upload.sh" "$sketch"
echo "--- monitor (Ctrl-C で終了) ---"
"$here/monitor.sh"
