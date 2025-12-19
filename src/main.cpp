#include <Wire.h>
#include "RTClib.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include "esp_system.h"

const char* ssid     = "ドラレコのSSID";
const char* password = "ドラレコのパスワード";
// const char *ssid = "自宅ルータ";   /* テスト用 */
// const char *password = "ルータパスワード";

Preferences preferences;

#define NVS_NAMESPACE "boot"
#define KEY_BUILD_ID  "build"

// RTCメモリ（電源OFFで消える）
RTC_DATA_ATTR bool alreadyExecuted = false;
const char* BUILD_ID = __DATE__ " " __TIME__;

RTC_DS1307 rtc;

// OLED 設定（128x32）
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

const int GND_PIN = 10;
const int VCC_PIN = 8;
const int SDA_PIN = 7;
const int SCL_PIN = 6;
const int BAT_ADC_PIN = 4;  // BAT 電圧測定用（GPIO5はADC2なのでWiFi使用時は使えないためGPIO4に変更）

void setSystemTimeFromRTC();
bool syncTimeToDVR();

void setup() {
  Serial.begin(115200);
  delay(1000);  // 書き込み時にRTCに時刻を書き込むためリセット起動が安定するまで待つ時間

  // I2Cデバイス 電源ON
  pinMode(GND_PIN, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)GND_PIN, GPIO_DRIVE_CAP_3);
  digitalWrite(GND_PIN, LOW);
  pinMode(VCC_PIN, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)VCC_PIN, GPIO_DRIVE_CAP_3);
  digitalWrite(VCC_PIN, HIGH);

  Wire.begin(SDA_PIN, SCL_PIN);

  // --- OLED 初期化 ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    while (1);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- RTC 初期化 ---
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    while (1);
  }

  preferences.begin(NVS_NAMESPACE, false);
  String savedBuild = preferences.getString(KEY_BUILD_ID, "default");

  Serial.println(savedBuild);
  Serial.println(BUILD_ID);

  if (!alreadyExecuted && savedBuild != BUILD_ID) {

    // 先にマーク（USB CDCによる多重リセット対策）
    alreadyExecuted = true;

    DateTime buildTime(__DATE__, __TIME__);
    DateTime setTime = buildTime + TimeSpan(20);  // 書き込むまでの時間の遅れを補正（平均的にビルド時刻から20秒遅れるため進める）

    // RTCに時刻を書き込む
    rtc.adjust(setTime);

    // 今回のビルドIDを保存
    preferences.putString(KEY_BUILD_ID, BUILD_ID);
    Serial.println("Write the time to the RTC.");
  }else{
    Serial.println("Use RTC value.");
  }

  preferences.end();

  setSystemTimeFromRTC();

  analogReadResolution(12);       // 0–4095
  analogSetAttenuation(ADC_11db); // 最大3.3V

  // ドラレコに接続
  display.clearDisplay();
  display.setTextSize(1);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    display.print(".");
    display.display();
  }

  if (!syncTimeToDVR()) {
    while (1);
  }
}

void loop() {
  // --- DS1307 時刻取得 ---
  DateTime now = rtc.now();

  // --- BAT 電圧 ---
  int raw = analogRead(BAT_ADC_PIN);
  float voltage = (float)raw * 3.3 / 4095.0;

  // --- 画面描画 ---
  display.clearDisplay();

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.printf("%02d:%02d:%02d", now.hour(), now.minute(), now.second()); // 時刻

  display.setCursor(0, 24);
  display.setTextSize(1);
  display.printf("%04d/%02d/%02d", now.year(), now.month(), now.day()); // 日付
  display.printf("  %.2f V", voltage); // 電圧

  display.display();

  delay(300);
}

void setSystemTimeFromRTC() {
  DateTime now = rtc.now();

  struct tm tm;
  tm.tm_year = now.year() - 1900;
  tm.tm_mon  = now.month() - 1;
  tm.tm_mday = now.day();
  tm.tm_hour = now.hour();
  tm.tm_min  = now.minute();
  tm.tm_sec  = now.second();
  tm.tm_isdst = -1;

  time_t t = mktime(&tm);

  struct timeval tv;
  tv.tv_sec = t;
  tv.tv_usec = 0;

  settimeofday(&tv, NULL);
}

bool syncTimeToDVR() {
  WiFiClient client;

  if (!client.connect("193.168.0.1", 80)) {
    display.println();
    display.println("Connection failed");
    display.display();
    return false;
  }

  // ESP32 の RTC から現在時刻取得（例）
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  // YYYYMMDDhhmmss を生成
  char dateStr[15];
  strftime(dateStr, sizeof(dateStr), "%Y%m%d%H%M%S", &timeinfo);

  // ms はとりあえず 0 にする
  int ms = millis() % 1000;

  // IMEIは適当でOK（16文字）
  const char* fakeIMEI = "1122334455667788";

  // JSON生成
  String jsonPayload = String("{\"date\":\"") + dateStr + "\","
                      "\"imei\":\"" + fakeIMEI + "\","
                      "\"ms\":" + ms + ","
                      "\"time_zone\":32400,"
                      "\"format\":\"dd\\/MM\\/yyyy HH:mm:ss\","
                      "\"lang\":\"ja_JP\"}";

  // HTTPリクエスト送信
  client.println("POST /vcam/cmd.cgi?cmd=API_SyncDate HTTP/1.1");
  client.println("Host: 193.168.0.1");
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(jsonPayload.length());
  client.println();
  client.println(jsonPayload);

  // レスポンス読み取り
  while (client.available() == 0) {
    delay(10);
  }

  while (client.available()) {
    String line = client.readStringUntil('\n');
    // display.println(line);
  }

  client.stop();

  return true;
}
