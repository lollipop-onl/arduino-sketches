# JoyStick → ブラウザ (Web Serial) 設計

作成日: 2026-06-02

## 目的
KY-023 アナログジョイスティックを UNO R4 WiFi に繋ぎ、その操作を
ブラウザで開いた静的ページ(GitHub Pages 配信)へ伝える。配線確認用の
可視化ダッシュと、操作が伝わる体験用のミニゲームを提供する。

## 通信方式
**Web Serial API (USB シリアル)**。
- GH Pages(https)/localhost で動作する静的ページ↔USB 機器の直結手段。
- WiFi 不要・追加配線不要。`navigator.serial.requestPort()` はクリック必須。
- 対応は Chromium 系(Chrome/Edge)のみ。非対応ブラウザは警告表示。

採用しなかった案: WiFi+WebSocket / WiFi+HTTP ポーリング
→ https(GH Pages)→ ws/http 平文で mixed content ブロックの懸念、IP 管理が必要。

## ハードウェア / 配線 (KY-023)
| KY-023 | UNO R4 | 備考 |
|--------|--------|------|
| GND    | GND    |      |
| +5V    | 5V     |      |
| VRx    | A0     | アナログ X |
| VRy    | A1     | アナログ Y |
| SW     | D2     | `INPUT_PULLUP`、押下=LOW |

## シリアルプロトコル
- `Serial.begin(115200)`
- 20ms 毎(~50Hz)に 1 行送出: `x,y,sw\n`
  - x,y: `analogRead` 0–1023
  - sw: 1=離す / 0=押下(`digitalRead(SW)` をそのまま)
  - 例: `512,498,1`

## Arduino 側
`sketches/joystick_serial/joystick_serial.ino`
- A0/A1 を `analogRead`、D2 を `INPUT_PULLUP` で `digitalRead`
- WiFi 不使用・認証不要

## ブラウザ側 (`docs/` = GH Pages 公開ルート)
- `index.html` / `style.css` / `app.js`
- [接続] ボタン → `requestPort()` → `open({baudRate:115200})`
  → `TextDecoderStream` + 行分割で 1 行ずつパース
- タブ 2 画面:
  - **ダッシュ**: XY ドット可視化、生値表示、ボタン状態 LED、
    中央キャリブレーション(中央オフセット記録)、raw ログ
  - **ミニゲーム**: キャンバス上のドットをジョイスティックで移動、
    ボタンで色変化。「操作が伝わる」体験。
- 非対応ブラウザ検出 → 警告。切断検知 → 再接続可能に。不正行はスキップ。

## デプロイ
GitHub Pages を `docs/` root に設定して公開。
リポジトリ: `lollipop-onl/arduino-sketches`。

## エラー処理
- `navigator.serial` 未定義 → 非対応警告。
- 読み取りループ内 try/catch、`reader.cancel()` で切断。
- パース失敗行はスキップしログに残す。
