// I2Cスキャナ: SCL/SDA に繋がったデバイスのアドレスを列挙する。
// LCD等の I2C アドレスを調べる用途。シリアルモニタ(115200)で結果を見る。
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  Wire.begin();
  Wire1.begin();
  Serial.println("I2C scanner start (Wire + Wire1)");
}

int scanBus(TwoWire &bus, const char *name) {
  int count = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      Serial.print(name);
      Serial.print(": found device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      count++;
    }
  }
  return count;
}

void loop() {
  int total = 0;
  total += scanBus(Wire, "Wire");
  total += scanBus(Wire1, "Wire1");
  if (total == 0) Serial.println("no I2C devices found on either bus");

  // 診断: Wire上の代表アドレスの endTransmission 戻り値を出す。
  //   2 = アドレスNACK(デバイスが応答しない/居ない/SDA-SCL逆)
  //   5 = timeout(バスがLowに固着 = 配線ショート/SDA or SCL がGND等に短絡)
  //   0 = 応答あり
  for (uint8_t a : {0x27, 0x3F, 0x20, 0x38}) {
    Wire.beginTransmission(a);
    uint8_t rc = Wire.endTransmission();
    Serial.print("Wire 0x");
    if (a < 16) Serial.print("0");
    Serial.print(a, HEX);
    Serial.print(" -> rc=");
    Serial.println(rc);
  }
  Serial.println("---");
  delay(3000);
}
