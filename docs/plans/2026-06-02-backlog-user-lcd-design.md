# backlog_user: Backlog myself をLCD表示

## 目的
Backlog API の `users/myself` を叩き、自分のユーザー名を I2C 1602 LCD に表示する。

## 構成
- スケッチ: `sketches/backlog_user/backlog_user.ino`(起動時1回実行)
- フロー: WiFi接続 → `WiFiSSLClient` で HTTPS GET → JSONパース → LCD表示
- エンドポイント: `GET https://<host>/api/v2/users/myself?apiKey=<key>`
- LCD: `LiquidCrystal_I2C(0x27, 16, 2)`。1行目 `Backlog user:`、2行目 `name`(16字切詰)
- JSON: ArduinoJson 7、`name` のみ filter 抽出
- レスポンス読み: `Connection: close`。ヘッダ空行までスキップ後、ボディ全読み→最初の `{` 以降を parse(chunked対応)

## env(mise.local.toml [env])
- `SECRET_SSID` / `SECRET_PASS` — WiFi
- `SECRET_BACKLOG_HOST` — FQDN 例 `xxxxx.backlog.com`
- `SECRET_BACKLOG_APIKEY` — 個人設定 > API で発行

## エラー表示(全てLCD2行目)
- `WiFi NG` / `Connect NG` / `Timeout` / `No response` / `HTTP <code>` / `Parse NG` / `No name`

## 決定事項
- space指定: フルhost(`.com`/`.jp`両対応)
- JSONパース: ArduinoJson lib
- 更新頻度: 起動時1回
- 日本語name: `name` をそのまま表示(HD44780は漢字/ひらがな不可→日本語名は文字化け)

## 制約
HD44780 は半角カタカナまで。`name` が漢字/ひらがなだと2行目が文字化けする。
回避は OLED(SSD1306)+ 日本語font が必要(別タスク)。
