# arduino-sketches

Arduino **UNO R4 WiFi** 専用のスケッチ集。arduino-cli + mise で操作する。

## ボード情報
- FQBN: `arduino:renesas_uno:unor4wifi`
- core: `arduino:renesas_uno`(インストール済み)
- WiFi ライブラリ: `WiFiS3`(core 同梱)
- ポートは `mise run ports` で確認。変わったら `mise.toml` の `ARDUINO_PORT` を更新する。

## 使用キット(配線説明はこの在庫から選ぶ)
- キット: **Keyestudio IoT Smart Living Learning Kit**(UNO R4 WiFi 同梱 / docs 上は KS0590〜KS0594 系)。
- **配線・部品の説明は必ず下記の同梱部品から構成する**。在庫に無い部品を要求しない(無い場合は明記して代替/購入を提案)。
- 主要モジュール: 超音波センサ ×1 / PIR モーションセンサ ×1 / RFID モジュール ×1 / ジョイスティックモジュール ×1(KY-023 相当 + ジョイスティックキャップ ×1) / MP3 モジュール ×1 / **I2C 1602 LCD ×1** / OLED ×1 / 4×4 メンブレンキーパッド ×1 / リレー ×1 / DHT11 ×1 / IR 受信モジュール ×1 / リモコン ×1。
- アクチュエータ: サーボ ×1 / ステッピングモータ ×1 + ドライブ基板 ×1 / DC モータ ×1 / ファン ×1 / スピーカ ×1 / 受動ブザー ×1 / 能動ブザー ×1。
- 表示・スイッチ系: 1桁デジタル管 ×1 / 4桁デジタル管 ×1 / ボタンモジュール ×6(ボタンキャップ: 黄 ×2/緑/白/赤/青) / 傾斜スイッチ ×2 / DIP スイッチ ×1。
- 単体部品: RGB LED ×1 / LED 赤・黄・緑・青 各 ×10 / 抵抗 220Ω・1KΩ・4.7KΩ・10KΩ 各 ×10 / 半固定 VR ×1 / フォトレジスタ ×2 / サーミスタ ×1 / 炎センサ ×1 / LM35 ×1 / NPN(S8050) ×1 / PNP(S8550) ×1 / 74HC595 ×1 / ダイオード ×2 / 電解コンデンサ ×5 / セラコン ×5。
- 配線材: ブレッドボード ×1 / ジャンパワイヤ / DuPont ワイヤ / USB-C ケーブル ×1 / 電池ボックス ×1。
- ユーザー準備(キット外): TF カード / 単3電池 ×6。
- **I2C 外部 pull-up はキットの 4.7KΩ 抵抗 ×10 を流用**(SDA→5V, SCL→5V)。専用 pull-up 部品は同梱されない。
- 既存スケッチとの対応: `joystick_*` = ジョイスティックモジュール、`lcd_*`/`backlog_*` = I2C 1602 LCD。

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
- `morse_keyer` — ボタン(D2)を電鍵として叩いたモールス信号を A-Z/0-9 に解読し I2C 1602 LCD に表示(複数ワード可)。短押し=`・`/長押し=`-`、無音長で文字・単語を確定。D3 で表示クリア(本体 RESET でも可)、LED(D4)とパッシブブザー(D5)が打鍵に同期(サイドトーン)。lib: `LiquidCrystal_I2C`。I2C 外部 pull-up 必須。さらに prosign(手順信号)を打つと相手局 BOT が半二重で応答する交信ごっこ機能あり: `KA`=交信開始→`QRV` / `AR`=通信文終わり→`R` / `SK`=交信終了→`73` / `BT`=区切り / 語境界の単独 `K`=どうぞ→直前メッセージをエコー。BOT 応答は LED+ブザーでモールス送出(ノンブロッキング)、LCD は `ME:`/`BOT:` の会話ログ型。LED は送信元で色分け: 自分の打鍵=D4(黄)、BOT 応答=D6(青)。

## docs/ (GitHub Pages)
- `docs/` を GH Pages root に公開。Web Serial API で USB シリアルを直読みする静的ページ。
- `joystick_serial` 用のダッシュボード+ミニゲーム。Chrome/Edge のみ(Web Serial 非対応ブラウザは警告)。
- URL: https://lollipop-onl.github.io/arduino-sketches/ ・ 詳細は `docs/README.md`。

## 配線をコードで管理 (Wokwi)
- 配線図を `sketches/<name>/diagram.json` に置く(部品と結線を JSON で記述 = 回路の Single Source of Truth)。`wokwi.toml` を併置すると実機なしでスケッチごとシミュレーションできる。
- いちばん手早い実行: [wokwi.com](https://wokwi.com) で UNO R4 WiFi を選び、`.ino` と `diagram.json` を貼って Run(クラウドでコンパイル → 配線ミスもその場で発見)。
- ローカル(VS Code 拡張 "Wokwi Simulator"): 先に `arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi --output-dir sketches/<name>/build sketches/<name>` で `build/` に成果物を出し、F1 → `Wokwi: Start Simulator`。`build/` は gitignore 済み。
- 部品 type / ピン名(例 `board-uno-r4-wifi`, `wokwi-lcd1602` の `pins:i2c`)は Wokwi エディタが補完・検証する。赤くなったら候補から選び直すだけ。
- 注意: sim では I2C の外部 pull-up を省略してよい(実機は必須)。LCD の SDA/SCL は `A4`/`A5` に結線(動かない時は基板の専用 `SDA`/`SCL` ピンへ。同一バス)。
- 現状 `morse_keyer` に `diagram.json` + `wokwi.toml` あり。

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
