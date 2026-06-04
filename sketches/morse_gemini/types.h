// 型定義(struct/enum)。
// Arduino の .ino は関数 prototype を「最後の #include 直後」に自動挿入する。
// 関数シグネチャで使う型(Button/QsoState/Prosign 等)が prototype より後で定義されると
// 「redeclared as different kind of symbol」で壊れる。型をヘッダに切り出し、最後の
// #include として読み込むことで、自動 prototype 挿入時に型が既知になるようにする。
#pragma once
#include <stdint.h>

// 1会話ターンのテキスト最大長(struct Turn が参照するためここで定義)。
static const int TURN_TEXT_MAX = 64;

// モールス符号表のエントリ。
struct MorseMap {
  const char* code;
  char ch;
};

// デバウンス付きボタン。
struct Button {
  uint8_t pin;
  bool pressed;
  bool lastRaw;
  unsigned long tChange;
};

// QSO 全体の状態。
enum QsoState {
  QSO_IDLE,        // 待機中
  QSO_ME_SENDING,  // 自局打鍵中
  QSO_API_WAIT,    // Gemini API 呼び出し中(ApiPhase で細分化)
  QSO_BOT_SENDING  // Gemini 返信をモールス送出中
};

// Gemini API 呼び出しのサブフェーズ(ノンブロッキング状態機械)。
enum ApiPhase {
  API_NONE,
  API_CONNECT,  // DNS+TLS 接続+リクエスト送信(TLS のみ短くブロック)
  API_WAIT,     // 初バイト到着待ち(ポーリング)
  API_READ,     // レスポンス読み取り(到着分のみ/ループ)
  API_PARSE     // パース+検証+送出開始(1回で完了)
};

// prosign(手順信号)。
enum Prosign { PRO_NONE, PRO_KA, PRO_AR, PRO_K, PRO_SK, PRO_BT };
struct ProsignMap {
  const char* code;
  Prosign id;
};

// 会話履歴1ターン(role=fromMe で user/model を区別)。
struct Turn {
  bool fromMe;                   // true=user(ME), false=model(BOT)
  char text[TURN_TEXT_MAX + 1];
};
