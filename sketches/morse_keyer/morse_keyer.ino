// モールス信号キーヤー: 1個のボタンを「電鍵」として叩き、打った符号を
// アルファベット/数字に解読して I2C 1602 LCD に表示する。単語の区切りも入る。
// もう1個のボタン(D3)で表示をクリアできる(本体の RESET ボタンでも消える)。
//
// 打ち方 (ストレートキー方式):
//   ・短く押す          -> 「・」(dot)        … 押下時間 < DASH_MS
//   ・長く押す          -> 「-」(dash)        … 押下時間 >= DASH_MS
//   ・少し止める        -> 1文字確定          … 無音 >= LETTER_GAP_MS
//   ・長めに止める      -> 単語の区切り(空白) … 無音 >= WORD_GAP_MS
//   未定義の符号を確定すると '?' になる。各定数はお好みで調整可。
//
// 交信ごっこ (prosign / 手順信号): 符号を文字間ギャップ無しで続けて打つと1つの
//   prosign として確定し、相手局 BOT がモールス(LED+ブザー)で半二重応答する。
//   KA(-.-.-)=交信開始 -> BOT "QRV" / AR(.-.-.)=通信文終わり -> BOT "R"
//   SK(...-.-)=交信終了 -> BOT "73"(送出後 IDLE) / BT(-...-)=区切り(空白挿入のみ)
//   K(-.-)=どうぞ: 語境界で単独の K を打つと送信権が渡り、BOT が直前メッセージを
//     エコー返信(本物同様、符号が文字 K と同じため文中の K は文字として扱う)。
//   BOT 送出中は電鍵入力を無視する(半二重)。クリア(D3)で BOT も即中断。
//
// LCD 表示 (会話ログ型):
//   1行目: "ME:" + 自分の解読テキスト(末尾13字)
//   2行目: 待機/入力中 = ">" + 入力中の符号(・-) + 右端に解読プレビュー1字
//          (prosign 候補は '*')。BOT 応答中 = "BOT:" + 送出中テキスト(1字ずつ伸びる)
//
// 配線 (ブレッドボードで組む。Wokwi 図 diagram.json と同一):
//   ブレッドボードの電源レールを 5V(+)/GND(-) として使い、各部品はそこへ落とす。
//     [電源]    UNO 5V -> (+)レール / UNO GND -> (-)レール
//   [ボタン]  電鍵:   片足 -> D2 / もう片足 -> (-)レール  (INPUT_PULLUP)
//             クリア: 片足 -> D3 / もう片足 -> (-)レール  (INPUT_PULLUP)
//   [LED]     自分(打鍵): D4 -> [220Ω] -> LED(黄)アノード, カソード -> (-)レール
//             (自分の打鍵に同期して点灯。基板上の LED_BUILTIN も同時に光る)
//   [LED]     相手(BOT) : D6 -> [220Ω] -> LED(青)アノード, カソード -> (-)レール
//             (BOT 応答のモールス送出に同期して点灯。送信元を色で区別)
//   [ブザー]  パッシブ(圧電)ブザー: + -> D5 / - -> (-)レール
//             (打鍵中=ドット/ダッシュ送出中だけ鳴る = サイドトーン。tone() 使用)
//   [LCD I2C] GND->(-)レール / VCC->(+)レール / SDA->A4 / SCL->A5
//     ※ UNO R4 は I2C 外部 pull-up 抵抗が必須。SDA と SCL を各 4.7kΩ で (+)レール
//        (=5V) へ吊る。無いと LCD が無反応になる(CLAUDE.md / i2c_scan 参照)。
//
// 制約: HD44780 はアルファベット/数字向け。日本語は表示できない。
#include <string.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);  // (address, cols, rows)

const uint8_t PIN_KEY = 2;    // 電鍵ボタン -> GND
const uint8_t PIN_CLEAR = 3;  // クリアボタン -> GND
const uint8_t PIN_LED_EXT = 4; // 外部 LED (+抵抗) -> GND ※PIN_LED は core 予約マクロ(=13)
const uint8_t PIN_BUZZER = 5; // パッシブ(圧電)ブザー + -> GND
const uint8_t PIN_LED_BOT = 6; // BOT 応答専用 LED (+抵抗) -> GND

// --- タイミング定数 [ms] (打ちやすさに合わせて調整する) ---
const unsigned long DEBOUNCE_MS = 20;      // チャタリング除去
const unsigned long DASH_MS = 250;         // これ以上の押下は「-」
const unsigned long LETTER_GAP_MS = 700;   // この無音で1文字確定
const unsigned long WORD_GAP_MS = 1500;    // この無音で単語区切り(空白)
const unsigned long LED_FLASH_MS = 80;     // 確定/クリア時の確認フラッシュ
const unsigned int BUZZER_HZ = 800;        // ブザー(サイドトーン)の高さ[Hz]

const int MSG_MAX = 64;  // 解読テキストの保持上限(超過分は先頭から捨てる)
const int SYM_MAX = 10;  // 1符号列の最大シンボル数(prosign を含むため拡張)

const unsigned long UNIT_MS = 120;  // BOT 送出の1単位長(dot=1, dash=3)

// --- モールス符号表 (A-Z, 0-9) ---
struct MorseMap {
  const char* code;
  char ch;
};
const MorseMap MORSE[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},
    {".", 'E'},     {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},
    {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'},   {".-..", 'L'},
    {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},   {".--.", 'P'},
    {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},
    {"-.--", 'Y'},  {"--..", 'Z'},  {"-----", '0'}, {".----", '1'},
    {"..---", '2'}, {"...--", '3'}, {"....-", '4'}, {".....", '5'},
    {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
};

// デバウンス付きボタン。pressed が確定状態(true=押下)。
struct Button {
  uint8_t pin;
  bool pressed;
  bool lastRaw;
  unsigned long tChange;
};

enum QsoState { QSO_IDLE, QSO_ME_SENDING, QSO_BOT_SENDING };

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
Prosign matchProsign(const char* sym) {
  for (const ProsignMap& p : PROSIGNS) {
    if (strcmp(p.code, sym) == 0) return p.id;
  }
  return PRO_NONE;
}

// --- 状態 ---
char message[MSG_MAX + 1];  // 解読済みテキスト(null 終端)
int msgLen = 0;
char symbol[SYM_MAX + 1];   // 入力中の符号(・-)
int symLen = 0;

unsigned long pressStart = 0;     // 電鍵を押し始めた時刻
unsigned long releaseTime = 0;    // 最後に離した時刻(無音判定の起点)
unsigned long ledFlashUntil = 0;  // この時刻まで LED を点ける(確認フラッシュ)
bool spaceAdded = true;           // 直近の無音で空白を入れ済みか(先頭抑止で true)
bool dirty = true;                // LCD 再描画が必要か

// BOT が送出中のテキスト(例 "QRV")とその進行位置。
char botText[24];        // 送出する応答テキスト(英大文字/数字/空白)
int  botCharIdx = 0;     // botText 内の現在文字
const char* botCode = "";// 現在文字の符号(MORSE から引く)
int  botMarkIdx = 0;     // botCode 内の現在シンボル
bool botOn = false;      // 今 ON(発音中)か
unsigned long botPhaseUntil = 0;  // 現フェーズの終了時刻

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

Button keyBtn = {PIN_KEY, false, false, 0};
Button clearBtn = {PIN_CLEAR, false, false, 0};

// 1=押下エッジ, -1=離しエッジ, 0=変化なし。
int updateButton(Button& b) {
  bool raw = (digitalRead(b.pin) == LOW);  // INPUT_PULLUP: 押下=LOW
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.tChange = millis();
  }
  if ((millis() - b.tChange) > DEBOUNCE_MS && raw != b.pressed) {
    b.pressed = raw;
    return raw ? 1 : -1;
  }
  return 0;
}

void setLed(bool on) {
  digitalWrite(PIN_LED_EXT, on ? HIGH : LOW);
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
}

// BOT 応答専用 LED(相手局の送出に同期)。自分の打鍵 LED(D4)とは別ピン。
void setBotLed(bool on) {
  digitalWrite(PIN_LED_BOT, on ? HIGH : LOW);
}

// 電鍵を押している間だけブザーを鳴らす(状態が変わった時だけ tone/noTone)。
void setBuzzer(bool on) {
  static bool cur = false;
  if (on == cur) return;
  cur = on;
  if (on) tone(PIN_BUZZER, BUZZER_HZ);
  else noTone(PIN_BUZZER);
}

// 符号(・-)を1文字に解読。未定義なら 0 を返す。
char decode(const char* code) {
  for (const MorseMap& m : MORSE) {
    if (strcmp(m.code, code) == 0) return m.ch;
  }
  return 0;
}

// 1文字 -> 符号列(・-)。未定義文字は "" を返す。
const char* encode(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;  // 小文字を大文字へ
  for (const MorseMap& m : MORSE) {
    if (m.ch == c) return m.code;
  }
  return "";  // 空白などは符号なし(語間ギャップで処理)
}

// 解読テキストへ1字追加(上限超過分は先頭から捨てる)。
void appendChar(char c) {
  if (msgLen >= MSG_MAX) {
    memmove(message, message + 1, MSG_MAX - 1);
    msgLen = MSG_MAX - 1;
  }
  message[msgLen++] = c;
  message[msgLen] = '\0';
}

void resetMessage() {
  msgLen = 0;
  message[0] = '\0';
  symLen = 0;
  symbol[0] = '\0';
  spaceAdded = true;   // 先頭に空白を入れない
  qso = QSO_IDLE;      // 状態を待機へ
  botOn = false;       // BOT 送出の ON 状態も解除(防御)
  setBuzzer(false);    // BOT 送出中の鳴動を止める
  setLed(false);
  setBotLed(false);    // BOT 応答 LED も消灯
}

// 2行を毎回フル幅(16字)で上書き → clear() 不要でチラつかない。
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

// BOT 送出を 1 ステップ進める。送出完了で IDLE へ戻す。
// LCD 2行目に「打ち終わった文字まで」を出すため botCharIdx を表示側で使う。
void tickBot(unsigned long now) {
  if (qso != QSO_BOT_SENDING) return;
  if (now < botPhaseUntil) return;  // 現フェーズ継続中

  if (botOn) {
    // ON 終了 → 要素間ギャップ(1単位)へ
    botOn = false;
    setBuzzer(false);
    setBotLed(false);
    botMarkIdx++;
    botPhaseUntil = now + UNIT_MS;  // 要素間
    return;
  }

  // OFF フェーズ明け: 次のシンボル/文字/語へ
  if (botCode[botMarkIdx] == '\0') {
    // 現文字を打ち終えた → 次の文字へ(直前に要素間 1u が経過済み)
    botCharIdx++;
    if (botText[botCharIdx] == '\0') {  // 全文字送出完了
      setState(QSO_IDLE);
      dirty = true;
      return;
    }
    if (botText[botCharIdx] == ' ') {
      // 空白を飛び越え次の実文字へ。語間は直前 1u と合わせ計 7u。
      botCharIdx++;
      if (botText[botCharIdx] == '\0') {  // 末尾が空白
        setState(QSO_IDLE);
        dirty = true;
        return;
      }
      botMarkIdx = 0;
      botCode = encode(botText[botCharIdx]);
      botPhaseUntil = now + UNIT_MS * 6;  // 語間(直前の要素間 1u + 6 = 計 7u)
      dirty = true;
      return;
    }
    botMarkIdx = 0;
    botCode = encode(botText[botCharIdx]);
    botPhaseUntil = now + UNIT_MS * 2;  // 文字間(直前の要素間 1u + 2 = 計 3u)
    dirty = true;  // 表示の文字数更新
    return;
  }

  // 次のシンボルを ON
  char mark = botCode[botMarkIdx];
  botOn = true;
  setBuzzer(true);
  setBotLed(true);
  botPhaseUntil = now + ((mark == '-') ? UNIT_MS * 3 : UNIT_MS);
}

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

void setup() {
  Serial.begin(115200);
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_CLEAR, INPUT_PULLUP);
  pinMode(PIN_LED_EXT, OUTPUT);
  pinMode(PIN_LED_BOT, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  setLed(false);
  setBotLed(false);
  noTone(PIN_BUZZER);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Morse Keyer");
  lcd.setCursor(0, 1);
  lcd.print("D2:key D3:clear");
  delay(1500);

  resetMessage();
  dirty = true;
}

void loop() {
  unsigned long now = millis();

  // --- クリアボタン: 押下で全消去 ---
  if (updateButton(clearBtn) == 1) {
    resetMessage();
    ledFlashUntil = now + LED_FLASH_MS;
    Serial.println("[clear]");
    dirty = true;
  }

  // --- 電鍵ボタン: BOT 送出中は入力を無視 ---
  if (qso != QSO_BOT_SENDING) {
    int e = updateButton(keyBtn);
    if (e == 1) {  // 押し始め
      if (qso == QSO_IDLE) setState(QSO_ME_SENDING);
      pressStart = now;
      spaceAdded = false;  // 新しい入力 → 次の長い無音で空白を入れてよい
      dirty = true;        // LED/プレビュー更新のため
    } else if (e == -1) {  // 離した → 押下時間で ・ か - を決める
      unsigned long dur = now - pressStart;
      char mark = (dur < DASH_MS) ? '.' : '-';
      if (symLen < SYM_MAX) {
        symbol[symLen++] = mark;
        symbol[symLen] = '\0';
      }
      releaseTime = now;
      Serial.print("mark ");
      Serial.println(mark);
      dirty = true;
    }

    // --- 無音による確定(電鍵を離している間だけ判定) ---
    if (!keyBtn.pressed) {
      if (symLen > 0 && (now - releaseTime) >= LETTER_GAP_MS) {
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
      } else if (symLen == 0 && msgLen > 0 && !spaceAdded &&
                 message[msgLen - 1] != ' ' &&
                 (now - releaseTime) >= WORD_GAP_MS) {
        // 単語区切り(空白を1つだけ)
        appendChar(' ');
        spaceAdded = true;
        Serial.println("[space]");
        dirty = true;
      }
    }
  }

  // --- BOT 送出中は BOT が LED/ブザーを駆動。電鍵入力は無視 ---
  if (qso == QSO_BOT_SENDING) {
    tickBot(now);
  } else {
    setLed(keyBtn.pressed || (now < ledFlashUntil));
    setBuzzer(keyBtn.pressed);
  }

  // --- 表示更新(変化があった時だけ → I2C 負荷とチラつきを抑える) ---
  if (dirty) {
    render();
    dirty = false;
  }
}
