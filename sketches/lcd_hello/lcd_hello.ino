// 16x2 パラレルLCD (HD44780 / LCM1602) にテキスト表示する。
// I2Cではなくパラレル接続。LiquidCrystal ライブラリを使う(4bitモード)。
//
// 配線 (LCDピン → Arduino UNO R4 WiFi):
//   1 VSS -> GND
//   2 VDD -> 5V
//   3 V0  -> コントラスト。まず GND に直結で試す(暗すぎ/真っ黒なら可変抵抗の中央へ)
//   4 RS  -> D12
//   5 RW  -> GND
//   6 E   -> D11
//   7-10 D0..D3 -> 未接続(4bitモードなので不要)
//   11 D4 -> D5
//   12 D5 -> D4
//   13 D6 -> D3
//   14 D7 -> D2
//   15 A  -> 5V (220Ωあれば挟む。バックライト+)
//   16 K  -> GND (バックライト-)
#include <LiquidCrystal.h>

// LiquidCrystal(rs, enable, d4, d5, d6, d7)
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);

void setup() {
  lcd.begin(16, 2);        // 16文字 x 2行
  lcd.setCursor(0, 0);     // 1行目 左端
  lcd.print("Hello, Arduino!");
  lcd.setCursor(0, 1);     // 2行目 左端
  lcd.print("UNO R4 WiFi");
}

void loop() {
}
