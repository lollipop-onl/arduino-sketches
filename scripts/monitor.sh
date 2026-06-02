#!/usr/bin/env bash
# シリアルモニタを開く。終了は Ctrl-C。
set -euo pipefail
arduino-cli monitor --port "$ARDUINO_PORT" --config "baudrate=${ARDUINO_BAUD}"
