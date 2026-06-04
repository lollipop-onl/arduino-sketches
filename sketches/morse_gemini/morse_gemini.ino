// モールス AI 交信機: 電鍵(D2)で打ったモールスをリアルタイム解読し、
// prosign でハンドオーバーすると解読テキストを Google Gemini API に送信。
// Gemini の返信をモールス(D6 LED + D5 ブザー)で打ち返す本格的な AI QSO 装置。
//
// 打ち方 (ストレートキー方式):
//   ・短く押す          -> 「・」(dot)        … 押下時間 < DASH_MS
//   ・長く押す          -> 「-」(dash)        … 押下時間 >= DASH_MS
//   ・少し止める        -> 1文字確定          … 無音 >= LETTER_GAP_MS
//   ・長めに止める      -> 単語の区切り(空白) … 無音 >= WORD_GAP_MS
//
// prosign(手順信号) -> Gemini トリガー対応表:
//   KA (-.-.-)  交信開始: 直近メッセージ(または「START QSO」)を送信 -> Gemini が開局挨拶
//   AR (.-.-.）通信文終わり: 直近メッセージを送信 -> Gemini が了解応答
//   K  (-.-)   どうぞ(語境界のみ): 直近メッセージを送信 -> Gemini が返答
//   SK (...-.- ) 交信終了: 直近メッセージ + 「END QSO」を送信 -> Gemini が 73 で締め
//   BT (-...-) 区切り: メッセージに空白を追加するだけ(Gemini 呼び出しなし)
//
// HTTPS 呼び出し中は LCD に "TX..." を表示して電鍵入力を無視(半二重)。
// Gemini 返信は D6(青) LED + D5 ブザーで非同期ノンブロッキング送出。
// D3 クリアボタン: メッセージ・トランスクリプトをリセット、BOT 送出を中断。
//
// WiFi 接続失敗時はローカル打鍵モードで動作継続。Gemini 呼び出しエラー時は
// BOT が "?" または "QRX" を送信してデグレードする。
//
// LCD 表示 (会話ログ型):
//   1行目: "ME:" + 解読テキストの末尾(13字)
//   2行目: 待機/入力中 = ">" + 入力中の符号(・-) + 右端に解読プレビュー1字
//          (prosign 候補は '*')。BOT 応答中 = "BOT:" + 送出中テキスト
//          Gemini 呼び出し中 = "TX..." (ブロッキング期間)
//
// 必要な env (mise.local.toml の [env]):
//   SECRET_SSID            WiFi の SSID
//   SECRET_PASS            WiFi パスワード
//   SECRET_GEMINI_APIKEY   Gemini API キー (Google AI Studio で発行)
//
// 配線 (ブレッドボードで組む。Wokwi 図 diagram.json と同一):
//   ブレッドボードの電源レールを 5V(+)/GND(-) として使い、各部品はそこへ落とす。
//     [電源]    UNO 5V -> (+)レール / UNO GND -> (-)レール
//   [ボタン]  電鍵:   片足 -> D2 / もう片足 -> (-)レール  (INPUT_PULLUP)
//             クリア: 片足 -> D3 / もう片足 -> (-)レール  (INPUT_PULLUP)
//   [LED]     自分(打鍵): D4 -> [220Ω] -> LED(黄)アノード, カソード -> (-)レール
//             (自分の打鍵に同期して点灯。基板上の LED_BUILTIN も同時に光る)
//   [LED]     相手(Gemini): D6 -> [220Ω] -> LED(青)アノード, カソード -> (-)レール
//             (Gemini 返信のモールス送出に同期して点灯。送信元を色で区別)
//   [ブザー]  パッシブ(圧電)ブザー: + -> D5 / - -> (-)レール
//             (打鍵中=ドット/ダッシュ送出中だけ鳴る = サイドトーン。tone() 使用)
//   [LCD I2C] GND->(-)レール / VCC->(+)レール / SDA->A4 / SCL->A5
//     ※ UNO R4 は I2C 外部 pull-up 抵抗が必須。SDA と SCL を各 4.7kΩ で (+)レール
//        (=5V) へ吊る。無いと LCD が無反応になる(CLAUDE.md / i2c_scan 参照)。
//
// 使用ライブラリ: LiquidCrystal_I2C / ArduinoJson (v7) / WiFiS3(core同梱)
// 制約: HD44780 はアルファベット/数字向け。日本語は表示できない。
//       Wokwi シミュレーション環境では WiFi/Gemini 呼び出しは動作しない。
//       ローカルの電鍵入力・LED/ブザー応答は sim でも確認できる。
#include <string.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiS3.h>
#include <ArduinoJson.h>
#include "arduino_secrets.h"

// --- WiFi / Gemini 接続定数 ---
const char* WIFI_SSID = SECRET_SSID;
const char* WIFI_PASS = SECRET_PASS;
// API キーはヘッダで渡す(URL に含めない → ログ漏洩を防ぐ)。
const char* GEMINI_HOST = "generativelanguage.googleapis.com";
#define GEMINI_MODEL    "gemini-2.5-flash"
#define GEMINI_PATH     "/v1beta/models/" GEMINI_MODEL ":generateContent"

// Gemini へ送るシステム指示。英語の文字列リテラルとして本体に埋め込む。
// 短い大文字英語 + ハム無線略語のみで返すよう厳命する。
static const char SYSTEM_INSTRUCTION[] =
    "You are an amateur radio operator in a CW (Morse code) QSO. "
    "Reply ONLY in short uppercase English using standard ham-radio "
    "abbreviations and Q-codes (CQ QTH QRZ RST 73 88 OM DE FB HW ES TU WX "
    "RIG ANT PSE R K AR SK BT etc.). "
    "Keep your reply under 8 words. "
    "Use ONLY characters A-Z, 0-9, and spaces — no lowercase, no punctuation, "
    "no emoji, no explanations. "
    "You are the far station; continue the QSO naturally.";

// --- ピン定義 ---
const uint8_t PIN_KEY      = 2;   // 電鍵ボタン -> GND (INPUT_PULLUP)
const uint8_t PIN_CLEAR    = 3;   // クリアボタン -> GND (INPUT_PULLUP)
const uint8_t PIN_LED_EXT  = 4;   // 自分打鍵 LED (黄, +220Ω) -> GND
const uint8_t PIN_BUZZER   = 5;   // パッシブブザー + -> GND
const uint8_t PIN_LED_BOT  = 6;   // Gemini 応答 LED (青, +220Ω) -> GND

// --- タイミング定数 [ms] ---
const unsigned long DEBOUNCE_MS   = 20;
const unsigned long DASH_MS       = 250;   // これ以上の押下は「-」
const unsigned long LETTER_GAP_MS = 700;   // この無音で1文字確定
const unsigned long WORD_GAP_MS   = 3000;  // この無音で単語区切り(初心者向けに長め)
const unsigned long LED_FLASH_MS  = 80;    // 確定/クリア確認フラッシュ
const unsigned int  BUZZER_HZ     = 800;   // サイドトーン周波数 [Hz]
const unsigned long UNIT_MS       = 120;   // BOT モールス送出の1単位長

// --- バッファサイズ ---
const int MSG_MAX       = 64;    // 解読テキスト保持上限(先頭から捨てる)
const int SYM_MAX       = 10;    // 1符号列の最大シンボル数
const int BOT_TEXT_MAX  = 64;    // Gemini 返信テキストの最大長
const int TRANSCRIPT_MAX = 384;  // 送受ログの最大長(古い方から捨てる)
const int REQ_BODY_MAX  = 1400;  // HTTP リクエストボディバッファ(eSys512+ePrompt600+雛形)
const int RESP_BUF_MAX  = 768;   // HTTP レスポンスボディバッファ

// --- モールス符号表 (A-Z, 0-9) ---
struct MorseMap {
  const char* code;
  char ch;
};
const MorseMap MORSE[] = {
    {".-",   'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..",  'D'},
    {".",    'E'}, {"..-.", 'F'}, {"--.",  'G'}, {"....", 'H'},
    {"..",   'I'}, {".---", 'J'}, {"-.-",  'K'}, {".-..", 'L'},
    {"--",   'M'}, {"-.",   'N'}, {"---",  'O'}, {".--.", 'P'},
    {"--.-", 'Q'}, {".-.",  'R'}, {"...",  'S'}, {"-",    'T'},
    {"..-",  'U'}, {"...-", 'V'}, {".--",  'W'}, {"-..-", 'X'},
    {"-.--", 'Y'}, {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
    {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
    {"---..", '8'}, {"----.", '9'},
};

// --- デバウンス付きボタン ---
struct Button {
  uint8_t pin;
  bool pressed;
  bool lastRaw;
  unsigned long tChange;
};

// --- QSO 状態機械 ---
enum QsoState {
  QSO_IDLE,         // 待機中
  QSO_ME_SENDING,   // 自局打鍵中
  QSO_API_WAIT,     // Gemini API 呼び出し中(ブロッキング)
  QSO_BOT_SENDING   // Gemini 返信をモールス送出中
};

// --- prosign(手順信号)テーブル ---
enum Prosign { PRO_NONE, PRO_KA, PRO_AR, PRO_K, PRO_SK, PRO_BT };
struct ProsignMap {
  const char* code;
  Prosign id;
};
const ProsignMap PROSIGNS[] = {
    {"-.-.-",  PRO_KA},  // 交信開始
    {".-.-.",  PRO_AR},  // 通信文終わり
    {"...-.-", PRO_SK},  // 交信終了
    {"-...-",  PRO_BT},  // 区切り
    // K(-.-)は文字 K と同符号 → 語境界でのみ PRO_K として扱う(下記参照)
};

// --- ファイルスコープ状態変数 ---
char message[MSG_MAX + 1];      // 解読済みテキスト(null 終端)
int  msgLen = 0;

char symbol[SYM_MAX + 1];       // 入力中の符号(・-)
int  symLen = 0;

// 送受ログ: "ME: .../BOT: ..." を蓄積し Gemini に文脈として渡す
char transcript[TRANSCRIPT_MAX + 1];
int  transcriptLen = 0;

char reqBody[REQ_BODY_MAX];     // HTTP リクエストボディ(ヒープを使わない)
char respBuf[RESP_BUF_MAX];     // HTTP レスポンスボディ

unsigned long pressStart   = 0;
unsigned long releaseTime  = 0;
unsigned long ledFlashUntil = 0;
bool spaceAdded = true;
bool dirty = true;
bool wifiOk = false;            // WiFi 接続済みフラグ
bool wifiReconnecting = false;  // ノンブロッキング再接続の進行中フラグ
unsigned long reconnectStart = 0;

// BOT(Gemini 返信)送出状態
char botText[BOT_TEXT_MAX + 1];
int  botCharIdx  = 0;
const char* botCode  = "";
int  botMarkIdx  = 0;
bool botOn       = false;
unsigned long botPhaseUntil = 0;
bool showBotReply = false;      // 受信(BOT応答)を次の打鍵開始まで2行目に残す

QsoState qso = QSO_IDLE;

Button keyBtn   = {PIN_KEY,   false, false, 0};
Button clearBtn = {PIN_CLEAR, false, false, 0};

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiSSLClient client;

// ============================================================
// ユーティリティ
// ============================================================

// 1=押下エッジ, -1=離しエッジ, 0=変化なし。
int updateButton(Button& b) {
  bool raw = (digitalRead(b.pin) == LOW);  // INPUT_PULLUP: 押下=LOW
  if (raw != b.lastRaw) {
    b.lastRaw  = raw;
    b.tChange  = millis();
  }
  if ((millis() - b.tChange) > DEBOUNCE_MS && raw != b.pressed) {
    b.pressed = raw;
    return raw ? 1 : -1;
  }
  return 0;
}

void setLed(bool on) {
  digitalWrite(PIN_LED_EXT,  on ? HIGH : LOW);
  digitalWrite(LED_BUILTIN,  on ? HIGH : LOW);
}

void setBotLed(bool on) {
  digitalWrite(PIN_LED_BOT, on ? HIGH : LOW);
}

// 状態が変わった時だけ tone/noTone する(余分な呼び出しを抑制)。
void setBuzzer(bool on) {
  static bool cur = false;
  if (on == cur) return;
  cur = on;
  if (on) tone(PIN_BUZZER, BUZZER_HZ);
  else    noTone(PIN_BUZZER);
}

// 符号(・-)を1文字に解読。未定義なら 0 を返す。
char decode(const char* code) {
  for (const MorseMap& m : MORSE) {
    if (strcmp(m.code, code) == 0) return m.ch;
  }
  return 0;
}

// 1文字 -> 符号列(・-)。未定義は "" を返す。
const char* encode(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  for (const MorseMap& m : MORSE) {
    if (m.ch == c) return m.code;
  }
  return "";
}

// prosign 識別子に変換。該当なしは PRO_NONE。
Prosign matchProsign(const char* sym) {
  for (const ProsignMap& p : PROSIGNS) {
    if (strcmp(p.code, sym) == 0) return p.id;
  }
  return PRO_NONE;
}

// 解読テキストへ1字追加(上限超過分は先頭から捨てる)。
void appendChar(char c) {
  if (msgLen >= MSG_MAX) {
    memmove(message, message + 1, MSG_MAX - 1);
    msgLen = MSG_MAX - 1;
  }
  message[msgLen++] = c;
  message[msgLen]   = '\0';
}

// トランスクリプトにテキストを追記(上限超過時は先頭から捨てる)。
void appendTranscript(const char* text) {
  int addLen = strlen(text);
  int needed = transcriptLen + addLen;
  if (needed >= TRANSCRIPT_MAX) {
    int shift = needed - TRANSCRIPT_MAX + 1;
    if (shift > transcriptLen) shift = transcriptLen;
    memmove(transcript, transcript + shift, transcriptLen - shift + 1);
    transcriptLen -= shift;
  }
  strncat(transcript, text, TRANSCRIPT_MAX - transcriptLen);
  transcriptLen = strlen(transcript);
}

// Gemini に渡す前に文字を正規化する:
//   大文字化 / [A-Z0-9 ] 以外を除去 / 連続空白を1つに / 前後トリム / 上限切り捨て。
void sanitizeForMorse(const char* src, char* dst, int dstMax) {
  int di = 0;
  bool lastSpace = true;  // 先頭空白を抑制するため true スタート
  for (int i = 0; src[i] != '\0' && di < dstMax - 1; i++) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      dst[di++] = c;
      lastSpace = false;
    } else if (c == ' ' && !lastSpace) {
      dst[di++] = ' ';
      lastSpace = true;
    }
  }
  // 末尾の空白を除く
  while (di > 0 && dst[di - 1] == ' ') di--;
  dst[di] = '\0';
}

// ============================================================
// 状態遷移
// ============================================================

void setState(QsoState next) {
  if (qso == next) return;
  Serial.print("state ");
  Serial.print((int)qso);
  Serial.print("->");
  Serial.println((int)next);
  qso   = next;
  dirty = true;
}

// ============================================================
// LCD 描画
// ============================================================

// 2行をフル幅(16字)で上書き → clear() 不要でチラつかない。
void render() {
  char line[17];

  // 1行目: "ME:" + 解読テキストの末尾(13字)
  int avail = 13;
  int start = (msgLen > avail) ? (msgLen - avail) : 0;
  snprintf(line, sizeof(line), "ME:%-13s", message + start);
  lcd.setCursor(0, 0);
  lcd.print(line);

  // 2行目: 状態で切替
  if (qso == QSO_API_WAIT) {
    snprintf(line, sizeof(line), "%-16s", "TX...");
  } else if (qso == QSO_BOT_SENDING) {
    // ":" = 応答(">" の入力中と見分く)。送出済みの末尾15字を追従表示。
    int shown = botCharIdx + 1;
    int start = (shown > 15) ? (shown - 15) : 0;
    char buf[16];
    int n = 0;
    for (int i = start; i < shown && botText[i] != '\0' && n < 15; i++) {
      buf[n++] = botText[i];
    }
    buf[n] = '\0';
    snprintf(line, sizeof(line), ":%-15s", buf);
  } else if (showBotReply && symLen == 0) {
    // 直前の受信(BOT応答)を打鍵開始まで残す(末尾15字追従)。
    int len = strlen(botText);
    int rstart = (len > 15) ? (len - 15) : 0;
    snprintf(line, sizeof(line), ":%-15s", botText + rstart);
  } else {
    // 入力中符号 + 右端プレビュー(従来)
    char preview = ' ';
    if (symLen > 0) {
      Prosign pro = matchProsign(symbol);
      if (pro != PRO_NONE) preview = '*';
      else { char c = decode(symbol); preview = c ? c : '?'; }
    }
    snprintf(line, sizeof(line), ">%-13s %c", symbol, preview);
  }
  lcd.setCursor(0, 1);
  lcd.print(line);

  // カーソルは ME: 行(1行目)の次に文字が入る位置で点滅。文字が確定するたび
  // 右へ動く → 符号が区切れて1字確定したことが見て分かる。受信表示中は出さない。
  if (qso != QSO_API_WAIT && qso != QSO_BOT_SENDING && !showBotReply) {
    int col = 3 + (msgLen > 13 ? 13 : msgLen);  // "ME:" の3字 + 末尾13字表示の右端
    if (col > 15) col = 15;
    lcd.setCursor(col, 0);
    lcd.blink();
  } else {
    lcd.noBlink();
  }
}

// ============================================================
// BOT(Gemini 返信)送出エンジン(ノンブロッキング)
// ============================================================

void botStart(const char* text) {
  showBotReply = false;  // 新応答の送出開始 → 前回の受信保持を解除
  strncpy(botText, text, sizeof(botText) - 1);
  botText[sizeof(botText) - 1] = '\0';
  botCharIdx    = 0;
  botMarkIdx    = 0;
  botOn         = false;
  botPhaseUntil = 0;
  botCode       = encode(botText[0]);
  Serial.print("bot-send ");
  Serial.println(botText);
  setState(QSO_BOT_SENDING);
}

// loop() から毎回呼ぶ。送出完了で IDLE へ戻す。
void tickBot(unsigned long now) {
  if (qso != QSO_BOT_SENDING) return;
  if (now < botPhaseUntil) return;

  if (botOn) {
    // ON 終了 → 要素間ギャップ(1単位)へ
    botOn = false;
    setBuzzer(false);
    setBotLed(false);
    botMarkIdx++;
    botPhaseUntil = now + UNIT_MS;
    return;
  }

  // OFF フェーズ明け: 次のシンボル/文字/語へ
  if (botCode[botMarkIdx] == '\0') {
    // 現文字を打ち終えた → 次の文字へ
    botCharIdx++;
    if (botText[botCharIdx] == '\0') {
      setState(QSO_IDLE);
      showBotReply = true;  // 受信を次の打鍵開始まで残す
      dirty = true;
      return;
    }
    if (botText[botCharIdx] == ' ') {
      // 空白: 語間(直前の要素間 1u + 6u = 計 7u)
      botCharIdx++;
      if (botText[botCharIdx] == '\0') {
        setState(QSO_IDLE);
        dirty = true;
        return;
      }
      botMarkIdx    = 0;
      botCode       = encode(botText[botCharIdx]);
      botPhaseUntil = now + UNIT_MS * 6;
      dirty = true;
      return;
    }
    botMarkIdx    = 0;
    botCode       = encode(botText[botCharIdx]);
    botPhaseUntil = now + UNIT_MS * 2;  // 文字間(計 3u)
    dirty = true;
    return;
  }

  // 次のシンボルを ON
  char mark = botCode[botMarkIdx];
  botOn = true;
  setBuzzer(true);
  setBotLed(true);
  botPhaseUntil = now + ((mark == '-') ? UNIT_MS * 3 : UNIT_MS);
}

// ============================================================
// WiFi 接続
// ============================================================

// WiFi 接続 + DHCP 待ち。成功したら true を返す。
// 失敗しても LCD に状態を出してから false を返す(動作継続)。
bool connectWifi() {
  lcd.setCursor(0, 0); lcd.print("Connecting WiFi ");
  lcd.setCursor(0, 1); lcd.print("                ");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      Serial.print("WiFi status=");
      Serial.println(WiFi.status());
      lcd.setCursor(0, 0); lcd.print("WiFi NG         ");
      lcd.setCursor(0, 1); lcd.print("Key mode only   ");
      delay(2000);
      return false;
    }
    delay(500);
  }
  Serial.print("WiFi associated, RSSI=");
  Serial.println(WiFi.RSSI());

  // DHCP 待ち
  t0 = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    if (millis() - t0 > 15000) {
      Serial.println("DHCP fail");
      lcd.setCursor(0, 0); lcd.print("No IP/DHCP      ");
      delay(2000);
      return false;
    }
    delay(500);
  }
  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

// WiFi の再接続チェック(loop() 先頭で呼ぶ)。
// 重要: delay() でブロックすると打鍵/BOT 送出中の millis() タイマが一斉に満了し、
// モールスのギャップが 0ms に潰れて符号が崩れる。よって IDLE 時のみ、かつ
// ノンブロッキング(WiFi.begin を1回叩いて以降は status をポーリング)で行う。
void ensureWifi() {
  if (!wifiOk) return;            // 起動時失敗した場合は再試行しない
  if (qso != QSO_IDLE) return;    // 打鍵/API/BOT 送出中はブロックを避け後回し
  if (WiFi.status() == WL_CONNECTED) { wifiReconnecting = false; return; }

  if (!wifiReconnecting) {
    Serial.println("WiFi dropped, reconnecting...");
    lcd.setCursor(0, 1); lcd.print("WiFi reconnect  ");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiReconnecting = true;
    reconnectStart = millis();
    return;
  }
  if (millis() - reconnectStart > 15000) {
    Serial.println("reconnect fail");
    wifiReconnecting = false;
    wifiOk = false;
  }
}

// ============================================================
// Gemini API 呼び出し(ブロッキング)
// ============================================================

// JSON 文字列をエスケープしながら dst に書き込む。
// 戻り値: 書き込んだバイト数(NUL 除く)。dst は dstMax バイト。
static int jsonEscape(const char* src, char* dst, int dstMax) {
  int di = 0;
  for (int i = 0; src[i] != '\0' && di < dstMax - 2; i++) {
    char c = src[i];
    if (c == '"'  || c == '\\') { dst[di++] = '\\'; dst[di++] = c;  }
    else if (c == '\n')          { dst[di++] = '\\'; dst[di++] = 'n'; }
    else if (c == '\r')          { dst[di++] = '\\'; dst[di++] = 'r'; }
    else                         { dst[di++] = c; }
  }
  dst[di] = '\0';
  return di;
}

// callGemini: prompt を Gemini に POST し、返信テキストを outText に書く。
// 成功すれば true。失敗 or ブロック時は outText に "?" を入れて false を返す。
// API キーは x-goog-api-key ヘッダで渡す(URL には含めない)。
bool callGemini(const char* prompt, char* outText, int outMax) {
  client.stop();  // 前回が half-open でも確実に閉じてから再接続(socket leak 防御)

  // WiFi 接続確認
  if (WiFi.status() != WL_CONNECTED) {
    strncpy(outText, "QRX", outMax);
    outText[outMax - 1] = '\0';
    return false;
  }

  // --- 1. JSON ボディを固定バッファに構築 ---
  // システム指示とユーザープロンプトをエスケープして埋め込む。
  // eSys/ePrompt はエスケープ後の余裕込み。256 だと SYSTEM_INSTRUCTION(約409B)や
  // トランスクリプト入りプロンプトが silently 切り捨てられ Gemini が文脈を失う。
  char eSys[512], ePrompt[600];
  jsonEscape(SYSTEM_INSTRUCTION, eSys,    sizeof(eSys));
  jsonEscape(prompt,             ePrompt, sizeof(ePrompt));

  int bodyLen = snprintf(reqBody, sizeof(reqBody),
    "{"
      "\"system_instruction\":{\"parts\":[{\"text\":\"%s\"}]},"
      "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"%s\"}]}],"
      "\"generationConfig\":{"
        "\"temperature\":0.8,"
        "\"maxOutputTokens\":200,"
        "\"thinkingConfig\":{\"thinkingBudget\":0}"
      "}"
    "}",
    eSys, ePrompt);

  if (bodyLen <= 0 || bodyLen >= (int)sizeof(reqBody)) {
    Serial.println("reqBody overflow");
    strncpy(outText, "AGN", outMax);  // '?'は符号表に無く無音 → 可聴な AGN(再送)
    outText[outMax - 1] = '\0';
    return false;
  }

  // --- 2. DNS 解決(TLS 失敗と切り分けるための先行チェック) ---
  IPAddress ip;
  if (!WiFi.hostByName(GEMINI_HOST, ip)) {
    Serial.println("DNS fail");
    strncpy(outText, "QRX", outMax);
    outText[outMax - 1] = '\0';
    return false;
  }
  Serial.print("DNS resolved: ");
  Serial.println(ip);

  // --- 3. TLS 接続 ---
  int rc = client.connect(GEMINI_HOST, 443);
  Serial.print("TLS connect rc=");
  Serial.println(rc);
  if (!rc) {
    strncpy(outText, "QRX", outMax);
    outText[outMax - 1] = '\0';
    return false;
  }
  // 各 blocking ストリーム読み(status 行/ヘッダスキップ)の上限を 5s に縛る。
  // 既定 1s だと遅延サーバで取り逃し、長すぎると遅いヘッダ送出でハングする。
  client.setTimeout(5000);

  // --- 4. HTTP/1.0 で POST(chunked レスポンスを避けるため 1.0) ---
  // API キーは x-goog-api-key ヘッダに入れる(URL・Serial に出さない)。
  client.print("POST ");
  client.print(GEMINI_PATH);
  client.println(" HTTP/1.0");
  client.print("Host: ");
  client.println(GEMINI_HOST);
  client.print("x-goog-api-key: ");
  client.println(SECRET_GEMINI_APIKEY);  // ← ヘッダのみ。Serial に print しない。
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(bodyLen);
  client.println("Connection: close");
  client.println();        // ヘッダ終端の空行
  client.print(reqBody);   // ボディ(末尾の \r\n は不要)

  // --- 5. レスポンス到着待ち(Gemini の推論 + ネット RTT を考慮して 20 秒) ---
  unsigned long t0 = millis();
  while (client.available() == 0) {
    if (millis() - t0 > 20000) {
      Serial.println("Gemini timeout");
      client.stop();
      strncpy(outText, "QRX", outMax);
      outText[outMax - 1] = '\0';
      return false;
    }
    if (!client.connected() && client.available() == 0) {
      Serial.println("Gemini disconnected before response");
      client.stop();
      strncpy(outText, "QRX", outMax);
      outText[outMax - 1] = '\0';
      return false;
    }
  }

  // --- 6. ステータス行確認 ---
  String statusLine = client.readStringUntil('\n');
  Serial.print("HTTP status: ");
  Serial.println(statusLine);
  int code = statusLine.substring(9, 12).toInt();
  if (code != 200) {
    char msg[32];
    snprintf(msg, sizeof(msg), "HTTP %d", code);
    Serial.println(msg);
    client.stop();
    strncpy(outText, "AGN", outMax);  // '?'は符号表に無く無音 → 可聴な AGN(再送)
    outText[outMax - 1] = '\0';
    return false;
  }

  // --- 7. ヘッダをスキップ(空行まで。wall-clock 5s で打ち切り) ---
  unsigned long th = millis();
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
    if (millis() - th > 5000) {
      Serial.println("header skip timeout");
      client.stop();
      strncpy(outText, "QRX", outMax);
      outText[outMax - 1] = '\0';
      return false;
    }
  }

  // --- 8. ボディを固定バッファへ全読み(HTTP/1.0 なので chunked なし) ---
  int n = 0;
  t0 = millis();
  while ((client.connected() || client.available()) &&
         n < (int)sizeof(respBuf) - 1) {
    if (client.available()) {
      respBuf[n++] = (char)client.read();
    } else if (millis() - t0 > 10000) {
      break;
    }
  }
  respBuf[n] = '\0';
  client.stop();
  Serial.print("resp bytes=");
  Serial.println(n);

  // --- 9. ArduinoJson v7 フィルタ付きパース ---
  JsonDocument filter;
  filter["candidates"][0]["content"]["parts"][0]["text"] = true;
  filter["candidates"][0]["finishReason"]                = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, respBuf, DeserializationOption::Filter(filter));
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    strncpy(outText, "AGN", outMax);  // '?'は符号表に無く無音 → 可聴な AGN(再送)
    outText[outMax - 1] = '\0';
    return false;
  }

  // null-safe ネストアクセス
  JsonArray cands = doc["candidates"].as<JsonArray>();
  if (cands.isNull() || cands.size() == 0) {
    Serial.println("no candidates (blocked?)");
    strncpy(outText, "AGN", outMax);  // '?'は符号表に無く無音 → 可聴な AGN(再送)
    outText[outMax - 1] = '\0';
    return false;
  }
  const char* text = cands[0]["content"]["parts"][0]["text"];
  if (text == nullptr) {
    Serial.println("no text field");
    strncpy(outText, "AGN", outMax);  // '?'は符号表に無く無音 → 可聴な AGN(再送)
    outText[outMax - 1] = '\0';
    return false;
  }

  Serial.print("Gemini raw: ");
  Serial.println(text);

  // --- 10. 返信テキストを正規化(大文字 + [A-Z0-9 ] + 48字上限) ---
  char sanitized[BOT_TEXT_MAX + 1];
  sanitizeForMorse(text, sanitized, sizeof(sanitized));
  if (sanitized[0] == '\0') {
    strncpy(sanitized, "AGN", sizeof(sanitized));  // 空応答 → 可聴な AGN(再送)
  }

  strncpy(outText, sanitized, outMax - 1);
  outText[outMax - 1] = '\0';
  Serial.print("Gemini sanitized: ");
  Serial.println(outText);
  return true;
}

// ============================================================
// Gemini 呼び出し + BOT 送出のラッパー
// ============================================================

// promptSuffix: メッセージの末尾に追加する文脈ヒント(例 "START QSO", "END QSO")。
// nullptr を渡すと message をそのまま使う。
void triggerGemini(const char* promptSuffix) {
  // API 待機中は再トリガーしない
  if (qso == QSO_API_WAIT || qso == QSO_BOT_SENDING) return;

  setState(QSO_API_WAIT);
  dirty = true;
  render();  // "TX..." をすぐ表示するため render を即時呼ぶ

  // プロンプト: トランスクリプト + 直前メッセージ + 文脈ヒント
  char prompt[512];
  int pLen = 0;
  if (transcriptLen > 0) {
    pLen += snprintf(prompt + pLen, sizeof(prompt) - pLen,
                     "QSO LOG:\n%s\n\nNEW: ", transcript);
  }
  if (msgLen > 0) {
    pLen += snprintf(prompt + pLen, sizeof(prompt) - pLen, "%s", message);
  }
  if (promptSuffix != nullptr) {
    pLen += snprintf(prompt + pLen, sizeof(prompt) - pLen,
                     (pLen > 0) ? " %s" : "%s", promptSuffix);
  }
  if (pLen == 0) {
    strncpy(prompt, "CQ DE ME", sizeof(prompt));
  }

  // トランスクリプトに自局のメッセージを追記
  if (msgLen > 0) {
    appendTranscript("ME: ");
    appendTranscript(message);
    appendTranscript("\n");
  }

  // Gemini 呼び出し(ブロッキング)
  char reply[BOT_TEXT_MAX + 1];
  callGemini(prompt, reply, sizeof(reply));

  // トランスクリプトに Gemini の返信を追記
  appendTranscript("BOT: ");
  appendTranscript(reply);
  appendTranscript("\n");

  // BOT 送出開始
  botStart(reply);
}

// ============================================================
// prosign ハンドラ
// ============================================================

void handleProsign(Prosign p) {
  switch (p) {
    case PRO_KA:  // 交信開始 -> Gemini に開局挨拶を求める
      triggerGemini("START QSO");
      break;
    case PRO_AR:  // 通信文終わり -> Gemini に了解応答を求める
      triggerGemini(nullptr);
      break;
    case PRO_K:   // どうぞ(語境界) -> Gemini に返答を求める
      triggerGemini(nullptr);
      break;
    case PRO_SK:  // 交信終了 -> Gemini に締めの 73 を求める
      triggerGemini("END QSO");
      // botStart 完了後に IDLE へ。トランスクリプトは次の KA まで保持する。
      break;
    case PRO_BT:  // 区切り: 空白を挿入するだけ(Gemini 呼び出しなし)
      if (msgLen > 0 && message[msgLen - 1] != ' ') appendChar(' ');
      break;
    default:
      break;
  }
}

// ============================================================
// リセット
// ============================================================

void resetMessage() {
  msgLen = 0;
  message[0] = '\0';
  symLen = 0;
  symbol[0] = '\0';
  transcriptLen = 0;
  transcript[0] = '\0';
  spaceAdded = true;
  qso = QSO_IDLE;
  botOn = false;
  showBotReply = false;
  setBuzzer(false);
  setLed(false);
  setBotLed(false);
}

// ============================================================
// setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  pinMode(PIN_KEY,     INPUT_PULLUP);
  pinMode(PIN_CLEAR,   INPUT_PULLUP);
  pinMode(PIN_LED_EXT, OUTPUT);
  pinMode(PIN_LED_BOT, OUTPUT);
  pinMode(PIN_BUZZER,  OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  setLed(false);
  setBotLed(false);
  noTone(PIN_BUZZER);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Morse AI QSO");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  delay(1000);

  // WiFi 接続
  wifiOk = connectWifi();
  if (wifiOk) {
    lcd.setCursor(0, 0); lcd.print("WiFi OK         ");
    lcd.setCursor(0, 1); lcd.print("KA to start QSO ");
    Serial.println("WiFi ready");
  } else {
    Serial.println("WiFi unavailable; keying only mode");
  }
  delay(1500);

  resetMessage();
  dirty = true;
}

void loop() {
  unsigned long now = millis();

  // WiFi 接続状態を維持(切断時は再接続を試みる)
  ensureWifi();

  // --- クリアボタン: 全消去 + BOT 中断 ---
  if (updateButton(clearBtn) == 1) {
    resetMessage();
    ledFlashUntil = now + LED_FLASH_MS;
    Serial.println("[clear]");
    dirty = true;
  }

  // --- 電鍵: API 待機中・BOT 送出中は無視 ---
  if (qso != QSO_BOT_SENDING && qso != QSO_API_WAIT) {
    int e = updateButton(keyBtn);
    if (e == 1) {  // 押し始め
      if (showBotReply) {  // 受信を見ていた状態から新規送信開始 → 自局メッセージをクリア
        showBotReply = false;
        msgLen = 0;
        message[0] = '\0';
      }
      if (qso == QSO_IDLE) setState(QSO_ME_SENDING);
      pressStart = now;
      spaceAdded = false;
      dirty = true;
    } else if (e == -1) {  // 離した → ・ か -
      unsigned long dur = now - pressStart;
      char mark = (dur < DASH_MS) ? '.' : '-';
      if (symLen < SYM_MAX) {
        symbol[symLen++] = mark;
        symbol[symLen]   = '\0';
      }
      releaseTime = now;
      Serial.print("mark ");
      Serial.println(mark);
      dirty = true;
    }

    // --- 無音による確定 ---
    if (!keyBtn.pressed) {
      if (symLen > 0 && (now - releaseTime) >= LETTER_GAP_MS) {
        Prosign pro = matchProsign(symbol);

        // 語境界での単独 K = 送信権渡し
        bool atWordBoundary = (msgLen == 0 || message[msgLen - 1] == ' ');
        bool isOver = (strcmp(symbol, "-.-") == 0 && atWordBoundary);

        if (pro != PRO_NONE || isOver) {
          Serial.print("prosign ");
          Serial.println(symbol);
          handleProsign(isOver ? PRO_K : pro);
        } else {
          char c   = decode(symbol);
          char out = c ? c : '?';
          Serial.print("letter ");
          Serial.print(symbol);
          Serial.print(" -> ");
          Serial.println(out);
          appendChar(out);
        }
        symLen      = 0;
        symbol[0]   = '\0';
        ledFlashUntil = now + LED_FLASH_MS;
        dirty = true;
      } else if (symLen == 0 && msgLen > 0 && !spaceAdded &&
                 message[msgLen - 1] != ' ' &&
                 (now - releaseTime) >= WORD_GAP_MS) {
        // 単語区切り
        appendChar(' ');
        spaceAdded = true;
        Serial.println("[space]");
        dirty = true;
      }
    }
  }

  // --- BOT 送出エンジン / 自局 LED・ブザー ---
  if (qso == QSO_BOT_SENDING) {
    tickBot(now);
  } else if (qso != QSO_API_WAIT) {
    setLed(keyBtn.pressed || (now < ledFlashUntil));
    setBuzzer(keyBtn.pressed);
  }

  // --- LCD 更新(変化があった時だけ) ---
  if (dirty) {
    render();
    dirty = false;
  }
}
