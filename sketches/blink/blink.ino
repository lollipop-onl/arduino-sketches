// Lチカ: 基板上のLEDを1秒ごとに点滅させる。
// 認証情報なしで動く、最初の動作確認用スケッチ。

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(1000);
  digitalWrite(LED_BUILTIN, LOW);
  delay(1000);
}
