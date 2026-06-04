// モールス AI 交信機 (morse_gemini): 電鍵(D2)で打ったモールスをリアルタイム解読し、
// prosign でハンドオーバーすると会話履歴を Google Gemini API に送信、AI の返信を
// モールス(D6 LED + D5 ブザー)で打ち返す CW QSO 装置。
//
// === 設計方針 (production 指向) ===
//  ・HTTPS 呼び出しは状態機械(QSO_API_WAIT + ApiPhase)で分割し、レスポンス待ち/読み取りを
//    ノンブロッキング化。呼び出し中も clearBtn / BOT 送出 tick / LCD 描画が応答する。
//    (TLS ハンドシェイクのみ WiFiS3 に非同期 API が無く ~1-3s 短くブロックする)
//  ・会話履歴は role/text の構造体リング(Turn[])で保持し、ArduinoJson で多ターンの
//    contents[] を組み立てる。JSON エスケープも ArduinoJson に委譲(手書き escape を排除)。
//  ・入力検証: HTTP ステータス / finishReason(SAFETY 等) / promptFeedback.blockReason を判定し、
//    使えない応答は CW の略語(QRX=待機 / AGN=再送)にデグレードする。
//  ・診断ログは LOG_LEVEL で詳細度を制御。API キーは決してログ・LCD に出さない。
//
// 打ち方 (ストレートキー方式):
//   ・短く押す      -> 「・」(dot)        … 押下時間 < DASH_MS
//   ・長く押す      -> 「-」(dash)        … 押下時間 >= DASH_MS
//   ・少し止める    -> 1文字確定          … 無音 >= LETTER_GAP_MS
//   ・長めに止める  -> 単語の区切り(空白) … 無音 >= WORD_GAP_MS
//
// prosign(手順信号) -> Gemini トリガー対応表:
//   KA (-.-.-)  交信開始: メッセージ(無ければ "CQ DE ME")+ "START QSO" を送信
//   AR (.-.-.)  通信文終わり: メッセージを送信 -> Gemini が了解応答
//   K  (-.-)    どうぞ(語境界のみ): メッセージを送信 -> Gemini が返答
//   SK (...-.-) 交信終了: メッセージ + "END QSO" を送信 -> Gemini が 73 で締め
//   BT (-...-)  区切り: メッセージに空白を追加するだけ(Gemini 呼び出しなし)
//
// クリアボタン(D3): 短押し=1字削除 / 長押し(>=LONG_CLEAR_MS)=全消去 + API/BOT 中断。
//
// LCD 表示 (会話ログ型):
//   1行目: "ME:" + 解読テキストの末尾(13字)。末尾に点滅カーソル(文字確定で右へ→区切れ可視化)。
//   2行目: 入力中 = ">" + 入力中の符号(・-) + 右端プレビュー1字(prosign 候補は '*')
//          受信保持 = ":" + 直前の BOT 応答(次の打鍵まで残す)
//          API 呼び出し中 = "TX connect/wait/recv..." (フェーズ表示)
//          BOT 応答送出中 = ":" + 送出中テキスト(末尾追従)
//
// 必要な env (mise.local.toml の [env]):
//   SECRET_SSID            WiFi の SSID
//   SECRET_PASS            WiFi パスワード
//   SECRET_GEMINI_APIKEY   Gemini API キー (Google AI Studio で発行)
//
// 配線 (ブレッドボードで組む。Wokwi 図 diagram.json と同一):
//     [電源]    UNO 5V -> (+)レール / UNO GND -> (-)レール
//   [ボタン]  電鍵:   片足 -> D2 / もう片足 -> (-)レール  (INPUT_PULLUP)
//             クリア: 片足 -> D3 / もう片足 -> (-)レール  (INPUT_PULLUP)
//   [LED]     自分(打鍵): D4 -> [220Ω] -> LED(黄)アノード, カソード -> (-)レール
//   [LED]     相手(Gemini): D6 -> [220Ω] -> LED(青)アノード, カソード -> (-)レール
//   [ブザー]  パッシブ(圧電)ブザー: + -> D5 / - -> (-)レール (サイドトーン)
//   [LCD I2C] GND->(-)レール / VCC->(+)レール / SDA->A4 / SCL->A5
//     ※ UNO R4 は I2C 外部 pull-up 抵抗が必須(SDA/SCL を各 4.7kΩ で +5V へ)。
//
// 使用ライブラリ: LiquidCrystal_I2C / ArduinoJson (v7) / WiFiS3(core同梱)
// 制約: HD44780 は英数字向け。日本語は表示できない。Wokwi sim は WiFi/Gemini 不可
//       (ローカル打鍵・LED/ブザー応答のみ確認可)。
#include <string.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiS3.h>
#include <ArduinoJson.h>
#include "types.h"  // struct/enum 定義(自動 prototype 対策で最後の #include に置く)
#include "arduino_secrets.h"

// ============================================================
// 診断ログ (LOG_LEVEL: 0=無, 1=ERR, 2=WARN, 3=INFO, 4=DEBUG)
//   API キーは決して出力しない。
// ============================================================
static const uint8_t LOG_LEVEL = 3;
static void logAt(uint8_t lv, const char* tag, const char* msg) {
  if (lv > LOG_LEVEL) return;
  Serial.print(tag);
  Serial.println(msg);
}
#define LOGE(m) logAt(1, "[E] ", (m))
#define LOGW(m) logAt(2, "[W] ", (m))
#define LOGI(m) logAt(3, "[I] ", (m))
#define LOGD(m) logAt(4, "[D] ", (m))

// ============================================================
// 設定値
// ============================================================
// --- WiFi / Gemini エンドポイント ---
static const char* const WIFI_SSID  = SECRET_SSID;
static const char* const WIFI_PASS  = SECRET_PASS;
static const char* const GEMINI_HOST = "generativelanguage.googleapis.com";
#define GEMINI_MODEL "gemini-2.5-flash"
#define GEMINI_PATH  "/v1beta/models/" GEMINI_MODEL ":generateContent"

// --- Gemini 生成パラメータ ---
static const float        GEMINI_TEMPERATURE = 0.8f;
static const unsigned int GEMINI_MAX_TOKENS  = 200;  // 短文 QSO には十分(thinking は無効)

// Gemini へのシステム指示(英語リテラル, 本体に埋め込む)。
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
static const uint8_t PIN_KEY     = 2;  // 電鍵ボタン -> GND (INPUT_PULLUP)
static const uint8_t PIN_CLEAR   = 3;  // クリアボタン -> GND (INPUT_PULLUP)
static const uint8_t PIN_LED_EXT = 4;  // 自分打鍵 LED (黄, +220Ω) -> GND
static const uint8_t PIN_BUZZER  = 5;  // パッシブブザー + -> GND
static const uint8_t PIN_LED_BOT = 6;  // Gemini 応答 LED (青, +220Ω) -> GND

// --- タイミング定数 [ms] ---
static const unsigned long DEBOUNCE_MS   = 20;
static const unsigned long DASH_MS       = 250;   // これ以上の押下は「-」
static const unsigned long LETTER_GAP_MS = 700;   // この無音で1文字確定
static const unsigned long WORD_GAP_MS   = 3000;  // この無音で単語区切り(初心者向けに長め)
static const unsigned long LED_FLASH_MS  = 80;    // 確定/クリア確認フラッシュ
static const unsigned long LONG_CLEAR_MS = 800;   // クリアボタン長押し=全削除の閾値
static const unsigned int  BUZZER_HZ     = 800;   // サイドトーン周波数 [Hz]
static const unsigned long UNIT_MS       = 120;   // BOT モールス送出の1単位長

// --- ネットワークタイムアウト [ms] ---
static const unsigned long HTTP_STREAM_TIMEOUT_MS = 5000;   // 各ストリーム読みの内部上限
static const unsigned long API_WAIT_TIMEOUT_MS    = 20000;  // 初バイト到着まで(推論+RTT)
static const unsigned long API_READ_TIMEOUT_MS    = 10000;  // 初バイト後の全読み
static const unsigned long WIFI_ASSOC_TIMEOUT_MS  = 20000;  // 起動時の関連付け
static const unsigned long WIFI_DHCP_TIMEOUT_MS   = 15000;  // 起動時の DHCP
static const unsigned long WIFI_RECONNECT_MS      = 15000;  // 再接続のあきらめ時間

// --- バッファサイズ ---
static const int MSG_MAX       = 64;    // 解読テキスト保持上限(先頭から捨てる)
static const int SYM_MAX       = 10;    // 1符号列の最大シンボル数
static const int BOT_TEXT_MAX  = 64;    // BOT 送出テキストの最大長
static const int MAX_TURNS     = 8;     // 保持する会話ターン数(古い方から捨てる)
// TURN_TEXT_MAX は types.h で定義(struct Turn が参照するため)。
static const int REQ_BODY_MAX  = 2048;  // HTTP リクエストボディ(JSON)
static const int RESP_BUF_MAX  = 2048;  // HTTP レスポンス全体(ヘッダ+ボディ)

// ============================================================
// モールス符号表 (A-Z, 0-9)  ※ struct MorseMap は types.h
// ============================================================
static const MorseMap MORSE[] = {
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

// 符号(・-)を1文字に解読。未定義なら 0。
static char decode(const char* code) {
  for (const MorseMap& m : MORSE) {
    if (strcmp(m.code, code) == 0) return m.ch;
  }
  return 0;
}

// 1文字 -> 符号列(・-)。未定義は ""。
static const char* encode(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  for (const MorseMap& m : MORSE) {
    if (m.ch == c) return m.code;
  }
  return "";
}

// ============================================================
// prosign(手順信号)  ※ enum Prosign / struct ProsignMap は types.h
// ============================================================
static const ProsignMap PROSIGNS[] = {
    {"-.-.-",  PRO_KA},  // 交信開始
    {".-.-.",  PRO_AR},  // 通信文終わり
    {"...-.-", PRO_SK},  // 交信終了
    {"-...-",  PRO_BT},  // 区切り
    // K(-.-)は文字 K と同符号 → 語境界でのみ PRO_K として扱う(loop 側で判定)
};
static Prosign matchProsign(const char* sym) {
  for (const ProsignMap& p : PROSIGNS) {
    if (strcmp(p.code, sym) == 0) return p.id;
  }
  return PRO_NONE;
}

// ============================================================
// デバウンス付きボタン  ※ struct Button は types.h
// ============================================================
// 1=押下エッジ, -1=離しエッジ, 0=変化なし。
static int updateButton(Button& b) {
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

// ============================================================
// 状態(ファイルスコープ)  ※ enum QsoState / ApiPhase は types.h
// ============================================================
// 解読中テキストと入力中符号
static char message[MSG_MAX + 1];
static int  msgLen = 0;
static char symbol[SYM_MAX + 1];
static int  symLen = 0;

// 会話履歴(role/text のリング)。Gemini の contents[] をここから組み立てる。
// ※ struct Turn は types.h
static Turn turns[MAX_TURNS];
static int  turnCount = 0;

// HTTP バッファ(ヒープ断片化を避けるためファイルスコープ固定確保)
static char reqBody[REQ_BODY_MAX];
static char respBuf[RESP_BUF_MAX];
static int  respLen = 0;

// 打鍵タイミング
static unsigned long pressStart      = 0;
static unsigned long releaseTime     = 0;
static unsigned long clearPressStart = 0;
static bool clearLongDone = false;
static unsigned long ledFlashUntil = 0;
static bool spaceAdded = true;
static bool dirty = true;
static bool showBotReply = false;  // 受信を次の打鍵開始まで2行目に残す

// WiFi
static bool wifiOk = false;
static bool wifiReconnecting = false;
static unsigned long reconnectStart = 0;

// BOT(Gemini 返信)送出エンジン
static char botText[BOT_TEXT_MAX + 1];
static int  botCharIdx = 0;
static const char* botCode = "";
static int  botMarkIdx = 0;
static bool botOn = false;
static unsigned long botPhaseUntil = 0;

// API 状態機械
static QsoState qso = QSO_IDLE;
static ApiPhase apiPhase = API_NONE;
static unsigned long apiPhaseStart = 0;

static Button keyBtn   = {PIN_KEY,   false, false, 0};
static Button clearBtn = {PIN_CLEAR, false, false, 0};

static LiquidCrystal_I2C lcd(0x27, 16, 2);
static WiFiSSLClient client;

// ============================================================
// 出力ヘルパ
// ============================================================
static void setLed(bool on) {
  digitalWrite(PIN_LED_EXT, on ? HIGH : LOW);
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
}
static void setBotLed(bool on) {
  digitalWrite(PIN_LED_BOT, on ? HIGH : LOW);
}
static void setBuzzer(bool on) {
  static bool cur = false;
  if (on == cur) return;
  cur = on;
  if (on) tone(PIN_BUZZER, BUZZER_HZ);
  else    noTone(PIN_BUZZER);
}

static void setState(QsoState next) {
  if (qso == next) return;
  if (LOG_LEVEL >= 4) {
    Serial.print("[D] state ");
    Serial.print((int)qso);
    Serial.print("->");
    Serial.println((int)next);
  }
  qso = next;
  dirty = true;
}

// ============================================================
// テキスト編集
// ============================================================
static void appendChar(char c) {
  if (msgLen >= MSG_MAX) {
    memmove(message, message + 1, MSG_MAX - 1);
    msgLen = MSG_MAX - 1;
  }
  message[msgLen++] = c;
  message[msgLen] = '\0';
}

// 1字削除(短押し): 入力中の符号があればまずそれを、無ければ確定済み末尾1字。
static void backspaceChar() {
  if (symLen > 0) {
    symLen = 0;
    symbol[0] = '\0';
  } else if (msgLen > 0) {
    msgLen--;
    message[msgLen] = '\0';
  }
}

// 会話履歴に1ターン追加(満杯なら最古を捨てる)。
static void addTurn(bool fromMe, const char* text) {
  if (turnCount >= MAX_TURNS) {
    for (int i = 1; i < MAX_TURNS; i++) turns[i - 1] = turns[i];
    turnCount = MAX_TURNS - 1;
  }
  turns[turnCount].fromMe = fromMe;
  strncpy(turns[turnCount].text, text, TURN_TEXT_MAX);
  turns[turnCount].text[TURN_TEXT_MAX] = '\0';
  turnCount++;
}

// Gemini に渡す前の正規化: 大文字化 / [A-Z0-9 ] 以外を除去 / 連続空白を1つに / 前後トリム。
static void sanitizeForMorse(const char* src, char* dst, int dstMax) {
  int di = 0;
  bool lastSpace = true;  // 先頭空白抑制
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
  while (di > 0 && dst[di - 1] == ' ') di--;
  dst[di] = '\0';
}

// ============================================================
// LCD 描画
// ============================================================
static void render() {
  char line[17];

  // 1行目: "ME:" + 解読テキストの末尾(13字)
  const int avail = 13;
  int start = (msgLen > avail) ? (msgLen - avail) : 0;
  snprintf(line, sizeof(line), "ME:%-13s", message + start);
  lcd.setCursor(0, 0);
  lcd.print(line);

  // 2行目: 状態で切替
  if (qso == QSO_API_WAIT) {
    const char* w = "TX...";
    switch (apiPhase) {
      case API_CONNECT: w = "TX connect...";  break;
      case API_WAIT:    w = "TX wait...";     break;
      case API_READ:    w = "TX recv...";     break;
      default:          w = "TX...";          break;
    }
    snprintf(line, sizeof(line), "%-16s", w);
  } else if (qso == QSO_BOT_SENDING) {
    // ":" = 応答。送出済みの末尾15字を追従表示。
    int shown = botCharIdx + 1;
    int s = (shown > 15) ? (shown - 15) : 0;
    char buf[16];
    int n = 0;
    for (int i = s; i < shown && botText[i] != '\0' && n < 15; i++) buf[n++] = botText[i];
    buf[n] = '\0';
    snprintf(line, sizeof(line), ":%-15s", buf);
  } else if (showBotReply && symLen == 0) {
    // 直前の受信を打鍵開始まで残す(末尾15字追従)。
    int len = strlen(botText);
    int rstart = (len > 15) ? (len - 15) : 0;
    snprintf(line, sizeof(line), ":%-15s", botText + rstart);
  } else {
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

  // カーソル: ME: 行の次に文字が入る位置で点滅。文字確定で右へ動き区切れが分かる。
  if (qso != QSO_API_WAIT && qso != QSO_BOT_SENDING && !showBotReply) {
    int col = 3 + (msgLen > 13 ? 13 : msgLen);
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
static void botStart(const char* text) {
  showBotReply = false;  // 新応答の送出開始 → 前回の受信保持を解除
  strncpy(botText, text, sizeof(botText) - 1);
  botText[sizeof(botText) - 1] = '\0';
  botCharIdx = 0;
  botMarkIdx = 0;
  botOn = false;
  botPhaseUntil = 0;
  botCode = encode(botText[0]);
  LOGI(botText);
  setState(QSO_BOT_SENDING);
}

static void tickBot(unsigned long now) {
  if (qso != QSO_BOT_SENDING) return;
  if (now < botPhaseUntil) return;

  if (botOn) {  // ON 終了 → 要素間ギャップ(1単位)
    botOn = false;
    setBuzzer(false);
    setBotLed(false);
    botMarkIdx++;
    botPhaseUntil = now + UNIT_MS;
    return;
  }

  if (botCode[botMarkIdx] == '\0') {  // 現文字を打ち終えた → 次の文字へ
    botCharIdx++;
    if (botText[botCharIdx] == '\0') {
      setState(QSO_IDLE);
      showBotReply = true;  // 受信を次の打鍵開始まで残す
      dirty = true;
      return;
    }
    if (botText[botCharIdx] == ' ') {  // 語間(直前の要素間 1u + 6u = 計 7u)
      botCharIdx++;
      if (botText[botCharIdx] == '\0') {
        setState(QSO_IDLE);
        showBotReply = true;
        dirty = true;
        return;
      }
      botMarkIdx = 0;
      botCode = encode(botText[botCharIdx]);
      botPhaseUntil = now + UNIT_MS * 6;
      dirty = true;
      return;
    }
    botMarkIdx = 0;
    botCode = encode(botText[botCharIdx]);
    botPhaseUntil = now + UNIT_MS * 2;  // 文字間(計 3u)
    dirty = true;
    return;
  }

  char mark = botCode[botMarkIdx];  // 次のシンボルを ON
  botOn = true;
  setBuzzer(true);
  setBotLed(true);
  botPhaseUntil = now + ((mark == '-') ? UNIT_MS * 3 : UNIT_MS);
}

// ============================================================
// WiFi 接続
// ============================================================
// 起動時の接続 + DHCP 待ち。成功で true。失敗しても LCD に状態を出して false を返す。
static bool connectWifi() {
  lcd.setCursor(0, 0); lcd.print("Connecting WiFi ");
  lcd.setCursor(0, 1); lcd.print("                ");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > WIFI_ASSOC_TIMEOUT_MS) {
      LOGW("WiFi assoc timeout");
      lcd.setCursor(0, 0); lcd.print("WiFi NG         ");
      lcd.setCursor(0, 1); lcd.print("Key mode only   ");
      delay(2000);
      return false;
    }
    delay(500);
  }
  t0 = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    if (millis() - t0 > WIFI_DHCP_TIMEOUT_MS) {
      LOGW("DHCP fail");
      lcd.setCursor(0, 0); lcd.print("No IP/DHCP      ");
      delay(2000);
      return false;
    }
    delay(500);
  }
  if (LOG_LEVEL >= 3) {
    Serial.print("[I] WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  }
  return true;
}

// 再接続チェック(loop 先頭で呼ぶ)。ブロックすると打鍵/送出の millis タイマが一斉に
// 満了し符号が崩れる → IDLE 時のみ、かつノンブロッキング(begin 1回 + status ポーリング)。
static void ensureWifi() {
  if (!wifiOk) return;
  if (qso != QSO_IDLE) return;
  if (WiFi.status() == WL_CONNECTED) { wifiReconnecting = false; return; }

  if (!wifiReconnecting) {
    LOGW("WiFi dropped, reconnecting...");
    lcd.setCursor(0, 1); lcd.print("WiFi reconnect  ");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiReconnecting = true;
    reconnectStart = millis();
    return;
  }
  if (millis() - reconnectStart > WIFI_RECONNECT_MS) {
    LOGE("WiFi reconnect failed");
    wifiReconnecting = false;
    wifiOk = false;
  }
}

// ============================================================
// Gemini API: リクエスト構築 / レスポンス解析(状態機械から呼ぶ)
// ============================================================
// 会話履歴から JSON リクエストボディを reqBody に構築。長さを返す(溢れたら -1)。
// JSON エスケープは ArduinoJson が処理する(手書き escape は不要)。
static int buildRequestBody() {
  JsonDocument doc;
  doc["system_instruction"]["parts"][0]["text"] = SYSTEM_INSTRUCTION;

  // contents[] は user で始まる必要がある(履歴シフトで先頭が model になり得るので調整)。
  int startIdx = 0;
  while (startIdx < turnCount && !turns[startIdx].fromMe) startIdx++;
  if (startIdx >= turnCount) startIdx = 0;

  JsonArray contents = doc["contents"].to<JsonArray>();
  for (int i = startIdx; i < turnCount; i++) {
    JsonObject t = contents.add<JsonObject>();
    t["role"] = turns[i].fromMe ? "user" : "model";
    t["parts"][0]["text"] = turns[i].text;
  }

  doc["generationConfig"]["temperature"]   = GEMINI_TEMPERATURE;
  doc["generationConfig"]["maxOutputTokens"] = GEMINI_MAX_TOKENS;
  doc["generationConfig"]["thinkingConfig"]["thinkingBudget"] = 0;  // 短文高速応答

  if (measureJson(doc) >= (size_t)sizeof(reqBody)) return -1;
  return (int)serializeJson(doc, reqBody, sizeof(reqBody));
}

// レスポンス先頭行 "HTTP/1.0 200 OK" からステータスコードを取る。失敗で -1。
static int httpStatusCode(const char* buf) {
  const char* sp = strchr(buf, ' ');
  if (!sp) return -1;
  return atoi(sp + 1);
}

// ヘッダ終端(空行)の直後 = ボディ先頭を返す。見つからなければ最初の '{'。
static const char* httpBody(const char* buf) {
  const char* p = strstr(buf, "\r\n\r\n");
  if (p) return p + 4;
  p = strstr(buf, "\n\n");
  if (p) return p + 2;
  return strchr(buf, '{');
}

// ============================================================
// API 状態機械
// ============================================================
// 失敗時の共通処理: TLS を閉じ、CW 略語コードを BOT 送出してデグレード。
//   code は "QRX"(待機/接続系) か "AGN"(再送/応答系)。履歴には残さない。
static void apiFail(const char* code) {
  client.stop();
  apiPhase = API_NONE;
  botStart(code);
}

// 接続 + リクエスト送信(TLS ハンドシェイク中のみ短くブロック)。成功で true。
static bool apiConnectAndSend() {
  int bodyLen = buildRequestBody();
  if (bodyLen < 0) { LOGE("request body overflow"); return false; }

  if (client.connect(GEMINI_HOST, 443) != 1) { LOGW("TLS connect failed"); return false; }
  client.setTimeout(HTTP_STREAM_TIMEOUT_MS);

  // HTTP/1.0 = chunked レスポンスを避ける。API キーはヘッダのみ(URL/ログに出さない)。
  client.print("POST ");
  client.print(GEMINI_PATH);
  client.println(" HTTP/1.0");
  client.print("Host: ");
  client.println(GEMINI_HOST);
  client.print("x-goog-api-key: ");
  client.println(SECRET_GEMINI_APIKEY);
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(bodyLen);
  client.println("Connection: close");
  client.println();
  client.print(reqBody);
  return true;
}

// レスポンスを解析し、検証を通れば BOT 送出を開始する。
static void apiParse() {
  // client.stop() は成功経路と apiFail() の片方だけが呼ぶ(二重 stop で TLS state を壊さない)。
  int code = httpStatusCode(respBuf);
  if (code != 200) {
    if (LOG_LEVEL >= 2) { Serial.print("[W] HTTP status "); Serial.println(code); }
    apiFail("AGN");
    return;
  }

  const char* body = httpBody(respBuf);
  if (!body) { LOGW("no response body"); apiFail("AGN"); return; }

  // candidates[0].content.parts[0].text と検証用フィールドだけ抽出。
  JsonDocument filter;
  filter["candidates"][0]["content"]["parts"][0]["text"] = true;
  filter["candidates"][0]["finishReason"] = true;
  filter["promptFeedback"]["blockReason"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) { LOGW(err.c_str()); apiFail("AGN"); return; }

  // 入力検証: プロンプト自体がブロックされた場合(安全フィルタ等)。
  const char* block = doc["promptFeedback"]["blockReason"];
  if (block) {
    if (LOG_LEVEL >= 2) { Serial.print("[W] prompt blocked: "); Serial.println(block); }
    apiFail("QRX");
    return;
  }

  const char* finish = doc["candidates"][0]["finishReason"];
  const char* text   = doc["candidates"][0]["content"]["parts"][0]["text"];

  // STOP / MAX_TOKENS は本文を採用(MAX_TOKENS は途中までだが短文なら実用上問題なし)。
  // SAFETY / RECITATION / OTHER は使わずデグレード。
  if (finish && strcmp(finish, "STOP") != 0 && strcmp(finish, "MAX_TOKENS") != 0) {
    if (LOG_LEVEL >= 2) { Serial.print("[W] finishReason "); Serial.println(finish); }
    apiFail("QRX");
    return;
  }
  if (!text) { LOGW("no text in response"); apiFail("AGN"); return; }

  char reply[BOT_TEXT_MAX + 1];
  sanitizeForMorse(text, reply, sizeof(reply));
  if (reply[0] == '\0') { LOGW("empty after sanitize"); apiFail("AGN"); return; }

  client.stop();          // 成功経路はここで明示的に閉じる
  addTurn(false, reply);  // 成功応答のみ履歴へ
  apiPhase = API_NONE;
  botStart(reply);
}

// loop から毎回呼ぶ。1フェーズずつ進める。
static void apiTick(unsigned long now) {
  if (qso != QSO_API_WAIT) return;

  switch (apiPhase) {
    case API_CONNECT:
      if (WiFi.status() != WL_CONNECTED) { LOGW("api: WiFi down"); apiFail("QRX"); return; }
      if (!apiConnectAndSend()) { apiFail("QRX"); return; }
      respLen = 0;
      respBuf[0] = '\0';
      apiPhaseStart = now;
      apiPhase = API_WAIT;
      dirty = true;
      LOGI("api: request sent");
      return;

    case API_WAIT:
      if (client.available() > 0) { apiPhase = API_READ; apiPhaseStart = now; dirty = true; return; }
      if (!client.connected())   { LOGW("api: closed before data"); apiFail("QRX"); return; }
      if (now - apiPhaseStart > API_WAIT_TIMEOUT_MS) { LOGW("api: wait timeout"); apiFail("QRX"); }
      return;

    case API_READ: {
      // 1ループあたり最大 256B 読み(loop の応答性を保つ)。
      int guard = 0;
      while (client.available() && respLen < RESP_BUF_MAX - 1 && guard++ < 256) {
        respBuf[respLen++] = (char)client.read();
      }
      respBuf[respLen] = '\0';
      bool full = (respLen >= RESP_BUF_MAX - 1);
      if (full || (!client.connected() && client.available() == 0)) {
        if (full) LOGW("api: response truncated (buffer full)");
        apiPhase = API_PARSE;
        return;
      }
      if (now - apiPhaseStart > API_READ_TIMEOUT_MS) {
        LOGW("api: read timeout (parsing partial)");
        apiPhase = API_PARSE;  // 取得済み分でパースを試みる
      }
      return;
    }

    case API_PARSE:
      apiParse();
      return;

    default:
      return;
  }
}

// 進行中の API 呼び出しを中断(クリア時など)。
static void abortApi() {
  if (apiPhase != API_NONE) {
    client.stop();
    apiPhase = API_NONE;
    if (qso == QSO_API_WAIT) setState(QSO_IDLE);  // dead state を残さない
    LOGI("api: aborted");
  }
}

// ============================================================
// Gemini トリガー / prosign ハンドラ
// ============================================================
// message + suffix から user ターンのテキストを作る(両方空なら "CQ DE ME")。
// suffix(START QSO/END QSO 等の文脈)を必ず残すため、先に suffix 分の領域を予約してから
// message を詰める(message が満杯でも suffix が無言で落ちないようにする)。
static void composeUserText(char* dst, int max, const char* suffix) {
  int suffixLen = (suffix && suffix[0]) ? (int)strlen(suffix) + 1 : 0;  // +1 は区切り空白
  int room = max - 1 - suffixLen;
  if (room < 0) room = 0;

  int n = 0;
  if (msgLen > 0) {
    int copy = (msgLen < room) ? msgLen : room;
    memcpy(dst, message, copy);
    n = copy;
  }
  dst[n] = '\0';

  if (suffixLen) {
    if (n > 0) dst[n++] = ' ';
    strncpy(dst + n, suffix, max - n - 1);
    dst[max - 1] = '\0';
    n = (int)strlen(dst);
  }
  if (n == 0) {
    strncpy(dst, "CQ DE ME", max - 1);
    dst[max - 1] = '\0';
  }
}

// 自局メッセージを履歴に積み、非同期 API 呼び出しを開始する(即時 return)。
static void triggerGemini(const char* suffix) {
  if (qso == QSO_API_WAIT || qso == QSO_BOT_SENDING) return;
  if (!wifiOk) { LOGW("gemini trigger but WiFi unavailable"); botStart("QRX"); return; }

  char userText[TURN_TEXT_MAX + 1];
  composeUserText(userText, sizeof(userText), suffix);
  addTurn(true, userText);

  setLed(false);
  setBuzzer(false);
  setState(QSO_API_WAIT);
  apiPhase = API_CONNECT;
  apiPhaseStart = millis();
  render();      // "TX connect..." を即表示
  dirty = false; // 同 loop での二重描画を防ぐ
}

static void handleProsign(Prosign p) {
  switch (p) {
    case PRO_KA: triggerGemini("START QSO"); break;  // 交信開始
    case PRO_AR: triggerGemini(nullptr);     break;  // 通信文終わり
    case PRO_K:  triggerGemini(nullptr);     break;  // どうぞ(語境界)
    case PRO_SK: triggerGemini("END QSO");   break;  // 交信終了
    case PRO_BT:                                      // 区切り: 空白を入れるだけ
      if (msgLen > 0 && message[msgLen - 1] != ' ') appendChar(' ');
      break;
    default: break;
  }
}

// ============================================================
// リセット
// ============================================================
static void resetMessage() {
  abortApi();
  msgLen = 0;
  message[0] = '\0';
  symLen = 0;
  symbol[0] = '\0';
  turnCount = 0;
  spaceAdded = true;
  showBotReply = false;
  qso = QSO_IDLE;
  botOn = false;
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

  wifiOk = connectWifi();
  if (wifiOk) {
    lcd.setCursor(0, 0); lcd.print("WiFi OK         ");
    lcd.setCursor(0, 1); lcd.print("KA to start QSO ");
    LOGI("ready");
  } else {
    LOGW("WiFi unavailable; keying-only mode");
  }
  delay(1500);

  resetMessage();
  dirty = true;
}

void loop() {
  unsigned long now = millis();

  ensureWifi();

  // --- クリアボタン: 短押し=1字削除 / 長押し=全消去 + API/BOT 中断 ---
  int ce = updateButton(clearBtn);
  if (ce == 1) {
    clearPressStart = now;
    clearLongDone = false;
  } else if (ce == -1) {
    if (!clearLongDone) {  // 長押し未発火 → 短押し扱いで1字削除
      backspaceChar();
      ledFlashUntil = now + LED_FLASH_MS;
      LOGD("backspace");
      dirty = true;
    }
    clearLongDone = false;
  }
  if (clearBtn.pressed && !clearLongDone && (now - clearPressStart) >= LONG_CLEAR_MS) {
    resetMessage();
    clearLongDone = true;
    ledFlashUntil = now + LED_FLASH_MS;
    LOGI("clear all");
    dirty = true;
  }

  // --- 電鍵: API 呼び出し中・BOT 送出中は無視(半二重) ---
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
        symbol[symLen] = '\0';
      }
      releaseTime = now;
      dirty = true;
    }

    // --- 無音による確定 ---
    if (!keyBtn.pressed) {
      if (symLen > 0 && (now - releaseTime) >= LETTER_GAP_MS) {
        Prosign pro = matchProsign(symbol);
        bool atWordBoundary = (msgLen == 0 || message[msgLen - 1] == ' ');
        bool isOver = (strcmp(symbol, "-.-") == 0 && atWordBoundary);  // 語境界の単独 K
        if (pro != PRO_NONE || isOver) {
          handleProsign(isOver ? PRO_K : pro);
        } else {
          char c = decode(symbol);
          appendChar(c ? c : '?');
        }
        symLen = 0;
        symbol[0] = '\0';
        ledFlashUntil = now + LED_FLASH_MS;
        dirty = true;
      } else if (symLen == 0 && msgLen > 0 && !spaceAdded &&
                 message[msgLen - 1] != ' ' &&
                 (now - releaseTime) >= WORD_GAP_MS) {
        appendChar(' ');
        spaceAdded = true;
        dirty = true;
      }
    }
  }

  // --- エンジン駆動 ---
  if (qso == QSO_BOT_SENDING) {
    tickBot(now);
  } else if (qso == QSO_API_WAIT) {
    apiTick(now);
  } else {
    setLed(keyBtn.pressed || (now < ledFlashUntil));
    setBuzzer(keyBtn.pressed);
  }

  if (dirty) {
    render();
    dirty = false;
  }
}
