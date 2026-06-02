# JoyStick → ブラウザ (Web Serial) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** KY-023 ジョイスティックの操作を UNO R4 のシリアル経由でブラウザ(GH Pages 静的ページ)へ伝え、可視化ダッシュとミニゲームで確認できるようにする。

**Architecture:** Arduino が `x,y,sw\n` CSV を 50Hz でシリアル送出。ブラウザは Web Serial API で行読み・パースし、2 タブ(ダッシュ/ミニゲーム)で表示。`docs/` を GH Pages root に公開。

**Tech Stack:** Arduino (renesas_uno) / Web Serial API / Vanilla JS + Canvas / GitHub Pages

検証方針: 厳密な自動テストは行わない。各タスクで「コンパイル成功」「ブラウザでの目視動作」など最低限の理論的検証のみ。

---

### Task 1: Arduino スケッチ

**Files:**
- Create: `sketches/joystick_serial/joystick_serial.ino`

**Step 1: 実装**

```cpp
// KY-023 joystick -> Serial CSV "x,y,sw"
// 配線: VRx->A0, VRy->A1, SW->D2(INPUT_PULLUP), +5V->5V, GND->GND
const int PIN_X = A0;
const int PIN_Y = A1;
const int PIN_SW = 2;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SW, INPUT_PULLUP);
}

void loop() {
  int x = analogRead(PIN_X);
  int y = analogRead(PIN_Y);
  int sw = digitalRead(PIN_SW); // 1=離す, 0=押下
  Serial.print(x);
  Serial.print(',');
  Serial.print(y);
  Serial.print(',');
  Serial.println(sw);
  delay(20);
}
```

**Step 2: コンパイル検証**

Run: `mise run compile joystick_serial`
Expected: コンパイル成功(エラー無し)

**Step 3: Commit**

```bash
git add sketches/joystick_serial/joystick_serial.ino
git commit -m "Add joystick_serial sketch: KY-023 to Serial CSV"
```

---

### Task 2: ページ骨組みと Web Serial 接続

**Files:**
- Create: `docs/index.html`
- Create: `docs/style.css`
- Create: `docs/app.js`

**Step 1: index.html**
- タイトル、非対応ブラウザ警告領域、[接続]/[切断]ボタン、タブ切替(ダッシュ/ゲーム)、各パネルのコンテナ、`app.js` 読み込み(`type="module"` 不要、通常 script)。

**Step 2: app.js 接続部**
- `navigator.serial` 未定義 → 警告表示。
- 接続: `requestPort()` → `open({baudRate:115200})` → `TextDecoderStream` でストリーム取得。
- 行バッファリング: 受信文字列を `\n` で分割、最後の不完全片はバッファに残す。
- 各行 `x,y,sw` をパースし `{x,y,sw}`(数値変換失敗行はスキップ)を共有状態へ。
- 切断: reader.cancel() + port.close()。

**Step 3: 検証**

Run: `python3 -m http.server -d docs 8000` で `http://localhost:8000` を開く。
Expected: ページ表示、非対応ブラウザなら警告、Chrome なら [接続] でポート選択ダイアログが出る。

**Step 4: Commit**

```bash
git add docs/index.html docs/style.css docs/app.js
git commit -m "Add Web Serial joystick page skeleton + connection"
```

---

### Task 3: ダッシュボード(可視化)

**Files:**
- Modify: `docs/app.js`, `docs/index.html`, `docs/style.css`

**Step 1: 実装**
- XY 可視化: 小さな正方形領域内に現在 X/Y を点で描画(0–1023 を領域にマップ)。
- 生値表示: `X:512 Y:498`、ボタン LED(押下=点灯)。
- キャリブ: [中央セット]で現在 X/Y を中央オフセットとして記録、点描画に反映。
- raw ログ: 直近 N 行を表示(古いものは破棄)。

**Step 2: 検証**: ローカルサーバ + 実機接続で、ジョイスティックを倒すと点が動き値が更新、ボタンで LED 点灯。

**Step 3: Commit**

```bash
git add docs/
git commit -m "Add joystick dashboard visualization"
```

---

### Task 4: ミニゲーム(操作体験)

**Files:**
- Modify: `docs/app.js`, `docs/index.html`, `docs/style.css`

**Step 1: 実装**
- Canvas にドット(自機)。X/Y の中央からの差分を速度にして毎フレーム移動、画面端でクランプ。
- ボタン押下でドットの色変化(操作が伝わる体験)。
- `requestAnimationFrame` ループで共有状態を参照して描画。

**Step 2: 検証**: 実機でジョイスティックを倒すとドットが動き、ボタンで色が変わる。

**Step 3: Commit**

```bash
git add docs/
git commit -m "Add joystick mini-game canvas demo"
```

---

### Task 5: GitHub Pages デプロイ

**Files:**
- Create: `docs/README.md`(任意、ページ説明と配線図)

**Step 1: Pages 有効化**
- `gh api -X POST repos/lollipop-onl/arduino-sketches/pages -f "source[branch]=main" -f "source[path]=/docs"`(既存なら PUT で更新)。失敗時は手順を提示(Settings → Pages → main / docs)。

**Step 2: 検証**: push 後 `https://lollipop-onl.github.io/arduino-sketches/` が表示されることを確認。

**Step 3: Commit & push**

```bash
git add docs/
git commit -m "Add Pages docs/README and enable GitHub Pages"
git push
```

---

### Task 6: CLAUDE.md 追記

**Files:**
- Modify: `CLAUDE.md`

**Step 1:** サンプル一覧に `joystick_serial` を追記、`docs/` の Web Serial ページと GH Pages URL を記載。

**Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: note joystick_serial and Web Serial page in CLAUDE.md"
```
