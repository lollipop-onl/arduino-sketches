// 稼働時間を HH:MM:SS.mmm でカウントアップ表示する。
// 接続は lcd_i2c と同じ(I2C, pull-up抵抗必須)。アドレスは 0x27。
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  lcd.init();
  lcd.backlight();
  lcd.setCursor(5, 0);     // 1行目中央あたり
  lcd.print("Uptime");
}

void loop() {
  unsigned long ms = millis();        // 起動からの経過ミリ秒
  unsigned long msec = ms % 1000;
  unsigned long sec  = (ms / 1000) % 60;
  unsigned long min  = (ms / 60000) % 60;
  unsigned long hour = ms / 3600000;  // 時間は桁あふれせず増える(約49.7日でmillisがrollover)

  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%03lu", hour, min, sec, msec);

  lcd.setCursor(2, 1);     // 2行目, "00:00:00.000"(12文字)を中央寄せ
  lcd.print(buf);          // clear()せず同じ位置に上書き → チラつかない
}
