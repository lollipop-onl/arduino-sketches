// Keyestudio KS0061 I2C 1602 LCD にテキスト表示する。
// LCD裏のI2Cバックパック(PCF8574)経由なので配線は4本だけ。
//
// 配線 (I2Cモジュール4ピン → Arduino UNO R4 WiFi):
//   GND -> GND
//   VCC -> 5V
//   SDA -> A4   (SDA と SCL を逆に挿すと表示されないので注意)
//   SCL -> A5
//
// アドレスは通常 0x27。違う場合は i2c_scan で調べた値に書き換える。
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);  // (address, cols, rows)

void setup() {
  lcd.init();          // LCD初期化
  lcd.backlight();     // バックライト点灯
  lcd.setCursor(0, 0); // 1行目 左端
  lcd.print("Hello, Arduino!");
  lcd.setCursor(0, 1); // 2行目 左端
  lcd.print("UNO R4 WiFi");
}

void loop() {
}
