// KY-023 ジョイスティックの X/Y/ボタンを I2C 1602 LCD に表示する。
// 併せて Serial にも "x,y,sw" CSV を送出するので docs/ の Web ページでも使える。
//
// 配線:
//   [KY-023]                 [LCD I2C (PCF8574)]
//   GND -> GND               GND -> GND
//   +5V -> 5V                VCC -> 5V
//   VRx -> A0                SDA -> A4
//   VRy -> A1                SCL -> A5
//   SW  -> D2 (INPUT_PULLUP)
//
//   ※ UNO R4 は I2C 外部 pull-up 抵抗が必須(SDA->5V, SCL->5V, 4.7k〜10k)。
//      無いと LCD が無反応になる(CLAUDE.md / i2c_scan 参照)。
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);  // (address, cols, rows)

const int PIN_X = A0;
const int PIN_Y = A1;
const int PIN_SW = 2;

const int LOW_TH = 400;   // 中央(約512)からの傾き判定しきい値
const int HIGH_TH = 624;
const unsigned long LCD_INTERVAL = 120;  // LCD 更新間隔[ms](I2C は遅い→間引く)

unsigned long lastLcd = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SW, INPUT_PULLUP);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("JoyStick LCD");
  lcd.setCursor(0, 1);
  lcd.print("connect...");
}

// X/Y のしきい値から方向ラベル(最大2文字: U/D/L/R 組合せ, 中央は C)を作る。
void direction(int x, int y, char *out) {
  int i = 0;
  if (y < LOW_TH) out[i++] = 'U';
  else if (y > HIGH_TH) out[i++] = 'D';
  if (x < LOW_TH) out[i++] = 'L';
  else if (x > HIGH_TH) out[i++] = 'R';
  if (i == 0) out[i++] = 'C';
  out[i] = '\0';
}

void loop() {
  int x = analogRead(PIN_X);
  int y = analogRead(PIN_Y);
  int sw = digitalRead(PIN_SW);  // 1=離す, 0=押下

  // Serial: Web ページ互換の CSV
  Serial.print(x);
  Serial.print(',');
  Serial.print(y);
  Serial.print(',');
  Serial.println(sw);

  // LCD: 間引いて更新(固定幅 + 空白 pad で残像防止)
  unsigned long now = millis();
  if (now - lastLcd >= LCD_INTERVAL) {
    lastLcd = now;
    char line[17];
    char dir[3];
    direction(x, y, dir);

    snprintf(line, sizeof(line), "X:%4d Y:%4d", x, y);  // 13文字
    lcd.setCursor(0, 0);
    lcd.print(line);
    lcd.print("   ");  // 残り桁を空白で消す

    snprintf(line, sizeof(line), "BTN:%s DIR:%s", sw == 0 ? "ON " : "OFF", dir);
    lcd.setCursor(0, 1);
    lcd.print(line);
    lcd.print("  ");
  }

  delay(20);
}
