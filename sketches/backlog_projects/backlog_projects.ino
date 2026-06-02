// Backlog のプロジェクト一覧を取得し、ボタンを押すたびに次のプロジェクトを
// I2C 1602 LCD に表示する。表示は "name (PROJECT_KEY)" 形式。
// 起動時に一度だけ一覧を取得してメモリに保持し、loop でボタン操作と表示を回す。
//
// 必要な env (mise.local.toml の [env]):
//   SECRET_SSID            WiFi SSID
//   SECRET_PASS            WiFi パスワード
//   SECRET_BACKLOG_HOST    Backlog の FQDN 例: "xxxxx.backlog.com"
//   SECRET_BACKLOG_APIKEY  Backlog 個人設定 > API で発行した API キー
//
// 配線:
//   I2C 1602 LCD: GND->GND / VCC->5V / SDA->A4 / SCL->A5
//     ※ UNO R4 は I2C 外部 pull-up 抵抗が必須 (SDA->5V, SCL->5V, 4.7k〜10k)。
//   ボタン: 片足->D2 / もう片足->GND (INPUT_PULLUP なので外部抵抗は不要)
//
// 制約: HD44780 は漢字/ひらがな不可。name が日本語だと文字化けする。
#include <WiFiS3.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include "arduino_secrets.h"

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;
const char* host = SECRET_BACKLOG_HOST;
const char* apiKey = SECRET_BACKLOG_APIKEY;

const int BUTTON_PIN = 2;       // ボタン -> D2 / GND
const int MAX_PROJECTS = 30;    // RAM 節約のため保持件数を上限でキャップ
const int LCD_COLS = 16;
const unsigned long SCROLL_MS = 350;  // 横スクロールの 1 文字送り間隔
const char* SCROLL_GAP = "   ";       // ループ時の区切り (末尾と先頭の間)

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiSSLClient client;

String names[MAX_PROJECTS];
String keys[MAX_PROJECTS];
int projectCount = 0;

int currentIndex = 0;
int lastButton = HIGH;
unsigned long lastPressMs = 0;

String scrollText;            // 現在表示中の "name (KEY)"
int scrollOffset = 0;
unsigned long lastScrollMs = 0;

void show(const char* l1, const char* l2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

// WiFi 接続 + プロジェクト一覧取得。成功で true。失敗時は LCD にエラー表示済み。
bool fetchProjects() {
  show("Backlog", "Connecting WiFi");

  // --- WiFi 接続 (status だけだと DHCP 前に抜けて IP=0.0.0.0 になるので IP も待つ) ---
  WiFi.begin(ssid, pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      Serial.print("WiFi status=");
      Serial.println(WiFi.status());
      show("Backlog", "WiFi NG");
      return false;
    }
    delay(500);
  }
  t0 = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    if (millis() - t0 > 15000) {
      show("Backlog", "No IP/DHCP");
      return false;
    }
    delay(500);
  }
  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());

  // --- TLS 接続 ---
  show("Backlog", "Loading...");
  if (!client.connect(host, 443)) {
    show("Backlog", "Connect NG");
    return false;
  }

  // --- HTTP/1.0 で要求 (chunked 転送を避け close 区切りにする) ---
  client.print("GET /api/v2/projects?apiKey=");
  client.print(apiKey);
  client.println(" HTTP/1.0");
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
      return false;
    }
    if (!client.connected() && client.available() == 0) {
      show("Backlog", "No response");
      client.stop();
      return false;
    }
  }

  // --- ステータス行 ("HTTP/1.0 200 OK") ---
  String status = client.readStringUntil('\n');
  Serial.println(status);
  int code = status.substring(9, 12).toInt();
  if (code != 200) {
    char msg[17];
    snprintf(msg, sizeof(msg), "HTTP %d", code);
    show("Backlog", msg);
    client.stop();
    return false;
  }

  // --- ヘッダをスキップ (空行 "\r" まで) ---
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  // --- ボディ (JSON 配列) を stream から直接パース ---
  // filter で name / projectKey だけ残し、大きな String 確保を避ける。
  // 配列 filter は要素 1 つ分を書くと全要素に適用される。
  JsonDocument filter;
  filter[0]["name"] = true;
  filter[0]["projectKey"] = true;

  client.setTimeout(10000);  // readBytes のブロック上限 (stream パース用)
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, client, DeserializationOption::Filter(filter));
  client.stop();
  if (err) {
    Serial.print("json err: ");
    Serial.println(err.c_str());
    show("Backlog", "Parse NG");
    return false;
  }

  // --- 先頭 MAX_PROJECTS 件を保存 ---
  projectCount = 0;
  for (JsonObject p : doc.as<JsonArray>()) {
    if (projectCount >= MAX_PROJECTS) break;
    const char* n = p["name"];
    const char* k = p["projectKey"];
    if (n == nullptr || k == nullptr) continue;
    names[projectCount] = String(n);
    keys[projectCount] = String(k);
    projectCount++;
  }
  Serial.print("projects: ");
  Serial.println(projectCount);

  if (projectCount == 0) {
    show("Backlog", "No projects");
    return false;
  }
  return true;
}

// currentIndex のプロジェクトを表示対象にセットする。
void selectProject() {
  scrollText = names[currentIndex] + " (" + keys[currentIndex] + ")";
  scrollOffset = 0;
  lastScrollMs = millis();

  // 2行目: 位置インジケータ "i/n"
  char pos[17];
  snprintf(pos, sizeof(pos), "%d/%d", currentIndex + 1, projectCount);

  lcd.clear();
  lcd.setCursor(0, 1);
  lcd.print(pos);
  // 1行目は updateScroll() が描画する。
  lcd.setCursor(0, 0);
  lcd.print(scrollText.substring(0, LCD_COLS));
}

// 1行目の横スクロールを進める (16字超のときのみ)。
void updateScroll() {
  if (scrollText.length() <= LCD_COLS) return;  // 収まるなら静止
  if (millis() - lastScrollMs < SCROLL_MS) return;
  lastScrollMs = millis();

  // gap を挟んで端をつないだ仮想テキストから 16 字窓を切り出す。
  String loopText = scrollText + SCROLL_GAP;
  int len = loopText.length();
  scrollOffset = (scrollOffset + 1) % len;

  String window = "";
  for (int i = 0; i < LCD_COLS; i++) {
    window += loopText.charAt((scrollOffset + i) % len);
  }
  lcd.setCursor(0, 0);
  lcd.print(window);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  if (!fetchProjects()) {
    return;  // エラーは LCD 表示済み。loop は何もしない。
  }
  selectProject();
}

void loop() {
  if (projectCount == 0) return;

  // --- ボタン: 押下エッジ (HIGH->LOW) + 200ms デバウンス ---
  int cur = digitalRead(BUTTON_PIN);
  if (lastButton == HIGH && cur == LOW && millis() - lastPressMs > 200) {
    lastPressMs = millis();
    currentIndex = (currentIndex + 1) % projectCount;
    selectProject();
  }
  lastButton = cur;

  updateScroll();
}
