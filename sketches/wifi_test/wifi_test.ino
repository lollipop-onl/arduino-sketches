// WiFi接続テスト: mise.local.toml の SECRET_SSID / SECRET_PASS を使って接続する。
// arduino_secrets.h は `mise run` 時に env から自動生成される(手で編集しない)。
#include <WiFiS3.h>
#include "arduino_secrets.h"

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
}

void loop() {
}
