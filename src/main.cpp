#include <Wire.h>
#include "RTClib.h"
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const char* dvr_ssid     = "ドラレコのSSID";
const char* dvr_password = "ドラレコのパスワード";
const char *ntp_ssid = "自宅ルータ";
const char *ntp_password = "ルータパスワード";

const char* ntpServer = "ntp.nict.jp";
const long  gmtOffset_sec = 9 * 3600;   // JST
const int   daylightOffset_sec = 0;
bool rtc_setup = false;
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
const int SW_PIN = 0;

bool syncTimeToDDPai();
void WifiConnect(const char *ssid, const char *password);

void setup() {
  Serial.begin(115200);
  pinMode(SW_PIN, INPUT_PULLUP);

  // SWが押されていなければ何もしない
  if (digitalRead(SW_PIN) == LOW) {
    rtc_setup = true;
  }
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
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // --- RTC 初期化 ---
  if (!rtc.begin()) {
    display.println("RTC not found!");
    display.display();
    while (1);
  }

  analogReadResolution(12);       // 0–4095
  analogSetAttenuation(ADC_11db); // 最大3.3V

  // WiFi接続
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);

  if ( rtc_setup == true ) {
    // NTP に接続
    WifiConnect(ntp_ssid, ntp_password);

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
      WiFi.disconnect(true);
      return;
    }

    // RTCへ書き込み
    rtc.adjust(DateTime(
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec
    ));
  } else {
    // Mini2 AP に接続
    WifiConnect(dvr_ssid, dvr_password);

    if (!syncTimeToDDPai()) {
      while (1);
    }
  }

  // WiFi切断
  WiFi.disconnect(true, false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
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

  display.setCursor(0, 16);
  display.setTextSize(2);
  display.printf("%02d/%02d", now.month(), now.day()); // 日付
  display.printf(" %.2f", voltage); // 電圧

  display.display();

  delay(300);
}

void WifiConnect(const char *ssid, const char *password) {
  display.println(ssid);
  display.display();
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    display.print(".");
    display.display();
  }
}

bool syncTimeToDDPai() {
  WiFiClient client;

  if (!client.connect("193.168.0.1", 80)) {
    display.println();
    display.println("Connection failed");
    display.display();
    return false;
  }

  // YYYYMMDDhhmmss を生成
  DateTime now = rtc.now();

  char dateStr[15];
  snprintf(dateStr, sizeof(dateStr), "%04d%02d%02d%02d%02d%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());

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
