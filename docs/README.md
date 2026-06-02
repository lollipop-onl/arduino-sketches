# JoyStick → Browser (Web Serial)

KY-023 ジョイスティックの操作を Arduino UNO R4 WiFi の USB シリアル経由で
ブラウザへ伝えるデモ。GitHub Pages で配信。

公開 URL: https://lollipop-onl.github.io/arduino-sketches/

## 使い方
1. `mise run upload joystick_serial` で UNO R4 にスケッチを書き込む。
2. Chrome / Edge(デスクトップ)で上記 URL を開く。
3. [接続] を押し、表示されたシリアルポートを選択。
4. **ダッシュボード**で XY/ボタンの生値を確認、**ミニゲーム**でドットを操作。

## 配線 (KY-023)
| KY-023 | UNO R4 |
|--------|--------|
| GND    | GND    |
| +5V    | 5V     |
| VRx    | A0     |
| VRy    | A1     |
| SW     | D2     |

## 仕組み
- Arduino が 20ms 毎に `x,y,sw\n`(CSV)をシリアル(115200)へ送出。
- ブラウザは Web Serial API でポートを開き、行単位でパースして描画。
- Web Serial は Chromium 系(Chrome/Edge)のみ対応。Safari/Firefox は非対応。

設計: [`plans/2026-06-02-joystick-web-serial-design.md`](plans/2026-06-02-joystick-web-serial-design.md)
