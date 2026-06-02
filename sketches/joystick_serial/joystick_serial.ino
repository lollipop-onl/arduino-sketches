// KY-023 joystick -> Serial CSV "x,y,sw"
// 配線: VRx->A0, VRy->A1, SW->D2(INPUT_PULLUP), +5V->5V, GND->GND
// 出力: 1行 "x,y,sw" を ~50Hz。x,y=0..1023, sw=1(離す)/0(押下)
const int PIN_X = A0;
const int PIN_Y = A1;
const int PIN_SW = 2;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SW, INPUT_PULLUP);
}

void loop() {
  int x = analogRead(PIN_X);
  int y = analogRead(PIN_Y);
  int sw = digitalRead(PIN_SW); // 1=離す, 0=押下
  Serial.print(x);
  Serial.print(',');
  Serial.print(y);
  Serial.print(',');
  Serial.println(sw);
  delay(20);
}
