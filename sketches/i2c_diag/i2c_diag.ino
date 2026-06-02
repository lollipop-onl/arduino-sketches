// I2Cバスの電気的状態を直接診断する。
// A4(SDA)/A5(SCL) を内部pull-up有効で読み、Low固着か判定する。
//   両方 HIGH(1) = 正常にHighへ上がる(pull-up効いてる/短絡なし)
//   どちらか LOW(0) = その線がGNDへ短絡 or 何かがLowに引いている
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
}

void loop() {
  // まず Wire を切ってピンを GPIO として内部pull-upで読む。
  pinMode(A4, INPUT_PULLUP);  // SDA
  pinMode(A5, INPUT_PULLUP);  // SCL
  delay(5);
  int sda = digitalRead(A4);
  int scl = digitalRead(A5);
  Serial.print("A4(SDA)=");
  Serial.print(sda);
  Serial.print("  A5(SCL)=");
  Serial.print(scl);
  Serial.print("  => ");
  if (sda == 1 && scl == 1) Serial.println("both HIGH (OK: 短絡なし)");
  else Serial.println("LOW detected (その線がGNDへ短絡 or Low固着)");

  // 続けて I2C スキャン(内部pull-upだけでも応答すれば拾える)。
  Wire.begin();
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  found 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      found++;
    }
  }
  if (found == 0) Serial.println("  (scan: no device)");
  Serial.println("---");
  delay(3000);
}
