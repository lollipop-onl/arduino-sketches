# arduino-sketches

Arduino **UNO R4 WiFi** 専用のスケッチ集。arduino-cli + mise で操作する。

## ボード情報
- FQBN: `arduino:renesas_uno:unor4wifi`
- core: `arduino:renesas_uno`(インストール済み)
- WiFi ライブラリ: `WiFiS3`(core 同梱)
- ポートは `mise run ports` で確認。変わったら `mise.toml` の `ARDUINO_PORT` を更新する。

## ディレクトリ
- `sketches/<name>/<name>.ino` — スケッチ本体(1ディレクトリ1スケッチ)
- `mise.toml` — 共有設定(FQBN/PORT)と操作コマンド(tasks)
- `mise.local.toml` — 認証情報など機密値の `[env]`(gitignore / 各自作成)
- `scripts/` — tasks の実体
- `**/arduino_secrets.h` — env から自動生成(gitignore / 手で編集しない)

## コマンド
- `mise run new <name>` — スケッチ雛形を作る
- `mise run compile <name>` — コンパイル
- `mise run upload <name>` — 書き込み
- `mise run dev <name>` — 書き込み → シリアルモニタ
- `mise run monitor` — シリアルモニタのみ
- `mise run secrets <name>` — arduino_secrets.h を再生成
- `mise run ports` — 接続中ボード/ポート一覧

## 認証情報の扱い
1. `cp mise.local.toml.example mise.local.toml` して値を埋める。
2. `[env]` に `SECRET_` で始まる変数を書く(例 `SECRET_SSID`, `SECRET_PASS`)。
3. `mise run compile/upload` 時に `scripts/gen-secrets.sh` が
   `SECRET_*` を `#define` 化して対象スケッチに `arduino_secrets.h` を生成する。
4. スケッチ側は `#include "arduino_secrets.h"` で `SECRET_SSID` 等を参照する。

新しい機密値は `SECRET_<NAME>` という名前で `mise.local.toml` に足すだけでよい。

## サンプル
- `blink` — Lチカ。認証不要、最初の動作確認用。
- `wifi_test` — WiFi 接続テスト。`SECRET_SSID`/`SECRET_PASS` が必要。

## Claude Code 向けメモ
- ライブラリ追加は `arduino-cli lib install "<name>"`。
- 新規スケッチは必ず `sketches/<name>/<name>.ino`(ディレクトリ名 = .ino 名)。
- 機密値をスケッチや `mise.toml` に直書きしない。必ず `SECRET_*` + `mise.local.toml`。
