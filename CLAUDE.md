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
- `lcd_i2c` — I2C 1602 LCD に文字表示(本命の I2C 構成)。
- `lcd_uptime` — 稼働時間を `HH:MM:SS.mmm` でカウントアップ表示。
- `i2c_scan` — I2C アドレススキャナ + `endTransmission` rc 診断(Wire/Wire1 両対応)。
- `i2c_diag` — SDA/SCL の Low 固着/短絡を内部 pull-up で判定。
- `lcd_hello` — パラレル接続版 1602(参考。I2C 不可なときのみ)。
- `backlog_user` — Backlog API `users/myself` を叩きユーザー名を LCD 表示。`SECRET_BACKLOG_HOST`/`SECRET_BACKLOG_APIKEY` + WiFi 必須。lib: `ArduinoJson`。
- `backlog_projects` — Backlog `projects` 一覧を取得し、ボタン(D2→GND)押下ごとに `name (KEY)` を横スクロール表示で巡回。同上 env + lib。
- `joystick_serial` — KY-023 ジョイスティックの X/Y/ボタンを `x,y,sw` CSV でシリアル送出(115200)。配線 VRx→A0/VRy→A1/SW→D2。`docs/` の Web Serial ページから読む。
- `joystick_lcd` — 上記 KY-023 を I2C 1602 LCD(0x27)に表示(1行目 `X/Y` 生値、2行目 `BTN`/方向)。Serial CSV も併出。lib: `LiquidCrystal_I2C`。I2C 外部 pull-up 必須。
- `morse_keyer` — ボタン(D2)を電鍵として叩いたモールス信号を A-Z/0-9 に解読し I2C 1602 LCD に表示(複数ワード可)。短押し=`・`/長押し=`-`、無音長で文字・単語を確定。D3 で表示クリア(本体 RESET でも可)、LED(D4)とパッシブブザー(D5)が打鍵に同期(サイドトーン)。lib: `LiquidCrystal_I2C`。I2C 外部 pull-up 必須。

## docs/ (GitHub Pages)
- `docs/` を GH Pages root に公開。Web Serial API で USB シリアルを直読みする静的ページ。
- `joystick_serial` 用のダッシュボード+ミニゲーム。Chrome/Edge のみ(Web Serial 非対応ブラウザは警告)。
- URL: https://lollipop-onl.github.io/arduino-sketches/ ・ 詳細は `docs/README.md`。

## I2C / LCD
- I2C バス: `Wire` = `A4`(SDA)/`A5`(SCL)。`Wire1` = Qwiic コネクタ**のみ**(ヘッダの SDA/SCL は Wire)。
- **UNO R4 は I2C 外部 pull-up 抵抗が必須**(`SDA→5V`, `SCL→5V`, 4.7k〜10k)。無いとバスが HIGH に上がれず `endTransmission` が `rc=5`(timeout)で全アドレス無応答になる。内部 pull-up だけでは不足。
- `rc` の読み: `0`=応答, `2`=NACK(デバイス無し/SDA-SCL逆), `5`=timeout(pull-up 無 or 線が GND へ短絡)。
- I2C 1602 LCD (Keyestudio KS0061 / PCF8574): addr `0x27`、lib `LiquidCrystal_I2C`、`lcd.init()` + `lcd.backlight()`。
- パラレル 1602 (HD44780 / "LCM1602" 単体): lib は要 `arduino-cli lib install "LiquidCrystal"`。コントラスト用に `V0` を可変抵抗(or GND)へ。
- 日本語: UTF-8 文字列の `print` は不可。**半角カタカナのみ** HD44780 バイトコードで送る(例 `コ`=`0xBA`)。漢字/ひらがなは OLED(SSD1306)+日本語 font library が必要。
- 配線が合ってるのに無反応なら、まず `i2c_scan` で rc を見る(`rc=5` なら pull-up を疑う)。

## Claude Code 向けメモ
- ライブラリ追加は `arduino-cli lib install "<name>"`。
- 新規スケッチは必ず `sketches/<name>/<name>.ino`(ディレクトリ名 = .ino 名)。
- 機密値をスケッチや `mise.toml` に直書きしない。必ず `SECRET_*` + `mise.local.toml`。
- `mise run` 以外で `arduino-cli` を直叩きするときは `$ARDUINO_PORT`/`$ARDUINO_FQBN` が未読込 → `-p`/`--fqbn` を明示する(mise の `[env]` は `mise run` 配下でのみ有効)。
- ヘッドレスでシリアル出力を読む: `arduino-cli monitor` をバックグラウンド + `sleep` + `head -n N` でファイルへキャプチャ。書込直後は板リセットで取り逃すので 1〜2 回再試行する。
