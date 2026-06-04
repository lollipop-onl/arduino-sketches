# morse_keyer 半二重交信(QSO)機能 実装計画

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 既存の morse_keyer に prosign 検出と相手局(BOT)の半二重応答を足し、1台で交信ごっこができるようにする。

**Architecture:** 符号列確定時に prosign 表を先に照合して状態マシン(IDLE / ME_SENDING / BOT_SENDING)を駆動する。BOT 応答は `millis()` 基準のノンブロッキング送出エンジンで LED(D4)+ ブザー(D5)に出す。LCD は会話ログ型(`ME:` / `BOT:`)に切り替える。

**Tech Stack:** Arduino C++(`arduino:renesas_uno:unor4wifi`)、`LiquidCrystal_I2C`、`Wire`。検証は `mise run compile morse_keyer` と実機 `mise run upload` + Serial。

**設計根拠:** `docs/plans/2026-06-04-morse-qso-design.md`

**対象ファイル(全タスク共通):** `sketches/morse_keyer/morse_keyer.ino` のみ

**テスト方針:** ホスト側テスト基盤が無いため、各タスクは「コンパイル通過」をゲートとし、ロジックは Serial ログで実機確認する。各タスク末でコミットする。

---

### Task 1: prosign 表と符号列照合の拡張

**Files:**
- Modify: `sketches/morse_keyer/morse_keyer.ino`(`SYM_MAX`、`MORSE` 周辺、`decode`)

**Step 1: SYM_MAX を拡張**

`SYM_MAX` を 7 → 10 にする(prosign 最長 `SK`=`...-.-` は 6 シンボル、余裕を持たせる)。

```cpp
const int SYM_MAX = 10;  // 1符号列の最大シンボル数(prosign を含むため拡張)
```

**Step 2: prosign 表を追加**

`MORSE[]` 定義の直後に追加する。

```cpp
// --- prosign(手順信号)表: 符号列 -> 識別子 ---
// 通常文字と区別するため decode より先に照合する。
enum Prosign { PRO_NONE, PRO_KA, PRO_AR, PRO_K, PRO_SK, PRO_BT };
struct ProsignMap {
  const char* code;
  Prosign id;
};
const ProsignMap PROSIGNS[] = {
    {"-.-.-", PRO_KA},   // 交信開始
    {".-.-.", PRO_AR},   // 通信文終わり
    {"...-.-", PRO_SK},  // 交信終了
    {"-...-", PRO_BT},   // 区切り
    // K(-.-)は文字 K と同符号 → 表には入れず文脈で判定(Task 4)
};

// 符号列を prosign 識別子に変換。該当なしは PRO_NONE。
Prosign matchProsign(const char* code) {
  for (const ProsignMap& p : PROSIGNS) {
    if (strcmp(p.code, code) == 0) return p.id;
  }
  return PRO_NONE;
}
```

**Step 3: コンパイル**

Run: `mise run compile morse_keyer`
Expected: PASS(`compile` 成功。未使用関数警告は許容)

**Step 4: コミット**

```bash
git add sketches/morse_keyer/morse_keyer.ino
git commit -m "morse_keyer: add prosign table and SYM_MAX expansion"
```

---

### Task 2: 状態マシンの導入

**Files:**
- Modify: `sketches/morse_keyer/morse_keyer.ino`(状態変数、`render` 分岐の足場)

**Step 1: 状態 enum と変数を追加**

`--- 状態 ---` ブロックに追加する。

```cpp
enum QsoState { QSO_IDLE, QSO_ME_SENDING, QSO_BOT_SENDING };
QsoState qso = QSO_IDLE;

// 状態遷移ヘルパ(Serial ログ付き)。
void setState(QsoState next) {
  if (qso == next) return;
  Serial.print("state ");
  Serial.print((int)qso);
  Serial.print("->");
  Serial.println((int)next);
  qso = next;
  dirty = true;
}
```

**Step 2: 打鍵開始で ME_SENDING へ**

`loop()` の電鍵押し始め(`e == 1`)分岐の先頭に追加する。

```cpp
  if (e == 1) {  // 押し始め
    if (qso == QSO_IDLE) setState(QSO_ME_SENDING);
    pressStart = now;
    ...
```

**Step 3: コンパイル**

Run: `mise run compile morse_keyer`
Expected: PASS

**Step 4: コミット**

```bash
git add sketches/morse_keyer/morse_keyer.ino
git commit -m "morse_keyer: introduce QSO state machine"
```

---

### Task 3: BOT ノンブロッキング送出エンジン

**Files:**
- Modify: `sketches/morse_keyer/morse_keyer.ino`(定数、送出状態、`tickBot`、`loop` 呼び出し)

**Step 1: 単位長と送出状態を追加**

```cpp
const unsigned long UNIT_MS = 120;  // BOT 送出の1単位長(dot=1, dash=3)

// BOT が送出中のテキスト(例 "QRV")とその進行位置。
char botText[24];        // 送出する応答テキスト(英大文字/数字/空白)
int  botCharIdx = 0;     // botText 内の現在文字
const char* botCode = "";// 現在文字の符号(MORSE から引く)
int  botMarkIdx = 0;     // botCode 内の現在シンボル
bool botOn = false;      // 今 ON(発音中)か
unsigned long botPhaseUntil = 0;  // 現フェーズの終了時刻
```

**Step 2: 文字→符号引き(decode の逆)を追加**

```cpp
// 1文字 -> 符号列(・-)。未定義文字は "" を返す。
const char* encode(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;  // 小文字を大文字へ
  for (const MorseMap& m : MORSE) {
    if (m.ch == c) return m.code;
  }
  return "";  // 空白などは符号なし(語間ギャップで処理)
}
```

**Step 3: BOT 送出開始関数を追加**

```cpp
// BOT 応答送出を開始する。text を 1 要素ずつ送る。
void botStart(const char* text) {
  strncpy(botText, text, sizeof(botText) - 1);
  botText[sizeof(botText) - 1] = '\0';
  botCharIdx = 0;
  botMarkIdx = 0;
  botOn = false;
  botPhaseUntil = 0;
  botCode = encode(botText[0]);
  Serial.print("bot-send ");
  Serial.println(botText);
  setState(QSO_BOT_SENDING);
}
```

**Step 4: BOT tick(ノンブロッキング進行)を追加**

```cpp
// BOT 送出を 1 ステップ進める。送出完了で IDLE へ戻す。
// LCD 2行目に「打ち終わった文字まで」を出すため botCharIdx を表示側で使う。
void tickBot(unsigned long now) {
  if (qso != QSO_BOT_SENDING) return;
  if (now < botPhaseUntil) return;  // 現フェーズ継続中

  if (botOn) {
    // ON 終了 → 要素間ギャップ(1単位)へ
    botOn = false;
    setBuzzer(false);
    setLed(false);
    botMarkIdx++;
    botPhaseUntil = now + UNIT_MS;  // 要素間
    return;
  }

  // OFF フェーズ明け: 次のシンボル/文字/語へ
  if (botCode[botMarkIdx] == '\0') {
    // 現文字を打ち終えた → 次の文字へ(文字間ギャップ込み)
    botCharIdx++;
    if (botText[botCharIdx] == '\0') {  // 全文字送出完了
      setState(QSO_IDLE);
      dirty = true;
      return;
    }
    botMarkIdx = 0;
    if (botText[botCharIdx] == ' ') {
      botCode = "";
      botPhaseUntil = now + UNIT_MS * 7;  // 語間
      return;
    }
    botCode = encode(botText[botCharIdx]);
    botPhaseUntil = now + UNIT_MS * 3;  // 文字間(直前の要素間1+追加で計3相当)
    dirty = true;  // 表示の文字数更新
    return;
  }

  // 次のシンボルを ON
  char mark = botCode[botMarkIdx];
  botOn = true;
  setBuzzer(true);
  setLed(true);
  botPhaseUntil = now + ((mark == '-') ? UNIT_MS * 3 : UNIT_MS);
}
```

**Step 5: loop から tickBot を呼ぶ**

`loop()` 末尾の LED/ブザー処理の **前** に、BOT 送出中は自前で LED/ブザーを握るため分岐する。

```cpp
  // --- BOT 送出中は BOT が LED/ブザーを駆動。電鍵入力は無視 ---
  if (qso == QSO_BOT_SENDING) {
    tickBot(now);
  } else {
    setLed(keyBtn.pressed || (now < ledFlashUntil));
    setBuzzer(keyBtn.pressed);
  }
```

(既存の `setLed(...)` / `setBuzzer(...)` 2行はこの分岐に置き換える)

**Step 6: BOT 送出中は電鍵を読まない**

`loop()` 冒頭、`updateButton(keyBtn)` を使う電鍵処理ブロック全体を `if (qso != QSO_BOT_SENDING) { ... }` で囲う(クリアボタンは常時有効のまま)。

**Step 7: コンパイル**

Run: `mise run compile morse_keyer`
Expected: PASS

**Step 8: コミット**

```bash
git add sketches/morse_keyer/morse_keyer.ino
git commit -m "morse_keyer: add non-blocking BOT morse transmitter"
```

---

### Task 4: prosign 確定 → 状態遷移と BOT 応答

**Files:**
- Modify: `sketches/morse_keyer/morse_keyer.ino`(符号列確定ブロック)

**Step 1: 確定処理を prosign 対応に書き換え**

`loop()` の「1文字確定」分岐(`symLen > 0 && (now - releaseTime) >= LETTER_GAP_MS`)の中身を次に置き換える。

```cpp
      // 符号列確定: prosign を先に照合する
      Prosign pro = matchProsign(symbol);

      // over 判定: 単独 "-.-" かつ語境界 = 送信権を渡す(本物の慣習)
      bool atWordBoundary = (msgLen == 0 || message[msgLen - 1] == ' ');
      bool isOver = (strcmp(symbol, "-.-") == 0 && atWordBoundary);

      if (pro != PRO_NONE || isOver) {
        Serial.print("prosign ");
        Serial.println(symbol);
        handleProsign(isOver ? PRO_K : pro);
      } else {
        char c = decode(symbol);
        char out = c ? c : '?';
        Serial.print("letter ");
        Serial.print(symbol);
        Serial.print(" -> ");
        Serial.println(out);
        appendChar(out);
      }
      symLen = 0;
      symbol[0] = '\0';
      ledFlashUntil = now + LED_FLASH_MS;
      dirty = true;
```

**Step 2: handleProsign を追加**

`loop()` の前に追加する。

```cpp
// prosign を解釈し、状態遷移と BOT 応答を起動する。
void handleProsign(Prosign p) {
  switch (p) {
    case PRO_KA:  // 交信開始 -> どうぞ
      botStart("QRV");
      break;
    case PRO_AR:  // 通信文終わり -> 了解
      botStart("R");
      break;
    case PRO_K:   // どうぞ -> 直前メッセージをエコー
      if (msgLen > 0) {
        botStart(message);
      } else {
        botStart("QRZ");  // 何も無ければ「誰か呼んだ?」
      }
      break;
    case PRO_SK:  // 交信終了 -> さよなら、IDLE へ
      botStart("73");
      // botStart 完了時に IDLE へ戻る
      break;
    case PRO_BT:  // 区切り: 空白を入れるだけ(BOT 応答なし)
      if (msgLen > 0 && message[msgLen - 1] != ' ') appendChar(' ');
      break;
    default:
      break;
  }
}
```

**Step 3: コンパイル**

Run: `mise run compile morse_keyer`
Expected: PASS

**Step 4: コミット**

```bash
git add sketches/morse_keyer/morse_keyer.ino
git commit -m "morse_keyer: route prosigns to state transitions and BOT replies"
```

---

### Task 5: LCD 会話ログ型レイアウト(案A)

**Files:**
- Modify: `sketches/morse_keyer/morse_keyer.ino`(`render`)

**Step 1: render を状態分岐に書き換え**

```cpp
void render() {
  char line[17];

  // 1行目: "ME:" + 解読テキストの末尾(13字)
  int avail = 13;
  int start = (msgLen > avail) ? (msgLen - avail) : 0;
  snprintf(line, sizeof(line), "ME:%-13s", message + start);
  lcd.setCursor(0, 0);
  lcd.print(line);

  // 2行目: 状態で切替
  if (qso == QSO_BOT_SENDING) {
    // "BOT:" + 送出済み文字まで(打つに連れ伸びる)
    int shown = botCharIdx + 1;  // 現在送出中の文字まで見せる
    char buf[13];
    int n = 0;
    for (int i = 0; i < shown && botText[i] != '\0' && n < 12; i++) buf[n++] = botText[i];
    buf[n] = '\0';
    snprintf(line, sizeof(line), "BOT:%-12s", buf);
  } else {
    // 入力中符号 + 右端プレビュー(従来)
    char preview = ' ';
    if (symLen > 0) {
      Prosign pro = matchProsign(symbol);
      if (pro != PRO_NONE) preview = '*';  // prosign 候補は '*'
      else { char c = decode(symbol); preview = c ? c : '?'; }
    }
    snprintf(line, sizeof(line), ">%-13s %c", symbol, preview);
  }
  lcd.setCursor(0, 1);
  lcd.print(line);
}
```

**Step 2: コンパイル**

Run: `mise run compile morse_keyer`
Expected: PASS

**Step 3: コミット**

```bash
git add sketches/morse_keyer/morse_keyer.ino
git commit -m "morse_keyer: conversation-log LCD layout (ME/BOT)"
```

---

### Task 6: クリアボタンで状態リセット・BOT 中断

**Files:**
- Modify: `sketches/morse_keyer/morse_keyer.ino`(クリアボタン分岐、`resetMessage`)

**Step 1: resetMessage で状態も初期化**

`resetMessage()` の末尾に追加する。

```cpp
void resetMessage() {
  ...
  spaceAdded = true;
  qso = QSO_IDLE;        // 状態を待機へ
  setBuzzer(false);      // BOT 送出中の鳴動を止める
  setLed(false);
}
```

**Step 2: コンパイル**

Run: `mise run compile morse_keyer`
Expected: PASS

**Step 3: コミット**

```bash
git add sketches/morse_keyer/morse_keyer.ino
git commit -m "morse_keyer: clear button resets QSO state and aborts BOT"
```

---

### Task 7: 実機統合確認 + ドキュメント更新

**Files:**
- Modify: `CLAUDE.md`(サンプル一覧の `morse_keyer` 説明を QSO 対応に更新)
- Modify: `sketches/morse_keyer/morse_keyer.ino`(冒頭コメントに prosign 操作を追記)

**Step 1: 実機へ書き込み**

Run: `mise run upload morse_keyer`
Expected: `Done` まで到達

**Step 2: Serial で挙動確認**

Run: `mise run monitor`(別端末)
操作と期待ログ:
- `-.-.-`(KA)を一塊で打つ → `prosign -.-.-` → `bot-send QRV` → LED/ブザーが QRV を送出 → `state 2->0`
- 適当な文字を打つ → `letter`、LCD 1行目 `ME:` に追記
- 語境界で単独 `-.-` → `prosign -.-`(over)→ 直前メッセージをエコー送出
- `...-.-`(SK)→ `bot-send 73` → 送出後 IDLE
- D3(クリア)→ `[clear]`、BOT 送出中なら即停止

**Step 3: LCD 目視**

- 1行目 `ME:...`、自分の打鍵で伸びる
- BOT 送出中は 2行目が `BOT:QRV` のように 1 字ずつ伸びる

**Step 4: コメントとドキュメント更新**

`.ino` 冒頭コメントに prosign 操作(KA/AR/K/SK/BT)を追記。`CLAUDE.md` の `morse_keyer` 行に「prosign を打つと相手局 BOT が半二重で応答(QRV/R/エコー/73)」を追記。

**Step 5: コミット**

```bash
git add sketches/morse_keyer/morse_keyer.ino CLAUDE.md
git commit -m "morse_keyer: document QSO prosign operation"
```

---

## 完了条件

- `mise run compile morse_keyer` がすべてのタスクで通る
- 実機で KA/AR/K/SK/BT が検出され、BOT が LED+ブザーで応答する
- LCD が `ME:`/`BOT:` の会話ログ型で表示される
- クリアボタンで状態・BOT 送出が初期化される
