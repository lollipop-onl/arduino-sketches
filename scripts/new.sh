#!/usr/bin/env bash
# 新しいスケッチの雛形を sketches/<name>/<name>.ino として作る。
set -euo pipefail

sketch="${1:?usage: new.sh <sketch>}"
dir="sketches/${sketch}"
[ -e "$dir" ] && { echo "error: already exists: $dir" >&2; exit 1; }

mkdir -p "$dir"
cat > "${dir}/${sketch}.ino" <<'INO'
// setup() は起動時に一度だけ実行される。
void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  Serial.println("hello");
}

// loop() は繰り返し実行される。
void loop() {
}
INO

echo "created: ${dir}/${sketch}.ino"
echo "次: mise run dev ${sketch}"
