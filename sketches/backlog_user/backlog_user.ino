// Backlog API の myself を叩いてユーザー名を I2C 1602 LCD に表示する。
// 起動時に一度だけ実行する: WiFi接続 -> HTTPS GET -> JSONパース -> LCD表示。
//
// 必要な env (mise.local.toml の [env]):
//   SECRET_SSID            WiFi SSID
//   SECRET_PASS            WiFi パスワード
//   SECRET_BACKLOG_HOST    Backlog の FQDN 例: "xxxxx.backlog.com"
//   SECRET_BACKLOG_APIKEY  Backlog 個人設定 > API で発行した API キー
//
// 配線 (I2C 1602 LCD 4ピン -> UNO R4 WiFi):
//   GND -> GND / VCC -> 5V / SDA -> A4 / SCL -> A5
//   ※ UNO R4 は I2C 外部 pull-up 抵抗が必須 (SDA->5V, SCL->5V, 4.7k〜10k)。
//
// 制約: HD44780 は漢字/ひらがな不可。name が日本語だと2行目は文字化けする。
#include <WiFiS3.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include "arduino_secrets.h"

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;
const char* host = SECRET_BACKLOG_HOST;
const char* apiKey = SECRET_BACKLOG_APIKEY;

LiquidCrystal_I2C lcd(0x27, 16, 2);  // (address, cols, rows)
WiFiSSLClient client;                 // Backlog API は HTTPS のみ

// LCD の2行へ表示する小ヘルパ。エラーも全てこれで可視化する。
void show(const char* l1, const char* l2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  show("Backlog", "Connecting WiFi");

  // --- WiFi 接続 (最大 ~20秒待つ) ---
  // status==WL_CONNECTED だけだと DHCP 完了前に抜けて IP=0.0.0.0 になることがある。
  // 有効な IP を取得するまで待つ。
  WiFi.begin(ssid, pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      Serial.print("WiFi status=");
      Serial.println(WiFi.status());
      show("Backlog", "WiFi NG");
      return;
    }
    delay(500);
  }
  Serial.print("associated, RSSI=");
  Serial.println(WiFi.RSSI());

  // DHCP で IP が割り当たるまで待つ (最大 ~15秒)。
  t0 = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    if (millis() - t0 > 15000) {
      Serial.println("no IP (DHCP fail)");
      show("Backlog", "No IP/DHCP");
      return;
    }
    delay(500);
  }
  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());

  // --- DNS 解決 (TLS 失敗と切り分けるため先に名前解決を確認) ---
  show("Backlog", "Resolving");
  Serial.print("host=[");
  Serial.print(host);
  Serial.println("]");
  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) {
    Serial.println("DNS FAIL");
    show("Backlog", "DNS NG");
    return;
  }
  Serial.print("resolved: ");
  Serial.println(ip);

  // --- TLS 接続 ---
  show("Backlog", "Connecting API");
  int rc = client.connect(host, 443);
  Serial.print("connect rc=");
  Serial.println(rc);  // 1=OK, それ以外は失敗
  if (!rc) {
    show("Backlog", "Connect NG");
    return;
  }

  // --- HTTP リクエスト送信 ---
  // apiKey はクエリ文字列で渡す。Connection: close でレスポンス完了を検知する。
  client.print("GET /api/v2/users/myself?apiKey=");
  client.print(apiKey);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(host);
  client.println("Connection: close");
  client.println();

  // --- レスポンス到着待ち (最大 ~10秒) ---
  t0 = millis();
  while (client.available() == 0) {
    if (millis() - t0 > 10000) {
      show("Backlog", "Timeout");
      client.stop();
      return;
    }
    if (!client.connected() && client.available() == 0) {
      show("Backlog", "No response");
      client.stop();
      return;
    }
  }

  // --- ステータス行を確認 ("HTTP/1.1 200 OK") ---
  String status = client.readStringUntil('\n');  // "HTTP/1.1 200 OK\r"
  Serial.println(status);
  int code = status.substring(9, 12).toInt();
  if (code != 200) {
    char msg[17];
    snprintf(msg, sizeof(msg), "HTTP %d", code);
    show("Backlog", msg);
    client.stop();
    return;
  }

  // --- ヘッダをスキップ (空行 "\r" まで読み飛ばす) ---
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  // --- ボディを全読み (chunked でも最初の '{' 以降を JSON とみなす) ---
  String body;
  t0 = millis();
  while (client.connected() || client.available()) {
    if (client.available()) {
      body += (char)client.read();
    } else if (millis() - t0 > 10000) {
      break;
    }
  }
  client.stop();

  int brace = body.indexOf('{');
  if (brace < 0) {
    show("Backlog", "No body");
    return;
  }

  // --- JSON パース (name フィールドだけ filter で抽出) ---
  JsonDocument filter;
  filter["name"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, body.c_str() + brace, DeserializationOption::Filter(filter));
  if (err) {
    show("Backlog", "Parse NG");
    Serial.println(err.c_str());
    return;
  }

  const char* name = doc["name"];
  if (name == nullptr) {
    show("Backlog", "No name");
    return;
  }
  Serial.print("name: ");
  Serial.println(name);

  // --- LCD 表示 (2行目は16文字に切り詰め) ---
  String line2 = String(name);
  if (line2.length() > 16) line2 = line2.substring(0, 16);
  show("Backlog user:", line2.c_str());
}

void loop() {
}
