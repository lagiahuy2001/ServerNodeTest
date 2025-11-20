#include <SPI.h>
#include <EthernetLarge.h>
#include <SSLClient.h>
#include "trust_anchors.h"
#include <NTPClient.h>
#include <EthernetUdp.h>
#include <SPIFFS.h>

// ====================== CẤU HÌNH ======================
#define CS_PIN       21
#define RST_PIN      4
#define SPI_SCK_PIN  18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
const char* DEVICE_KEY = "ESP_UNIT_001";        // THAY ĐỔI CHO TỪNG THIẾT BỊ

const char* API_HOST = "servernodetest-eiq5.onrender.com";
const int   API_PORT = 443;
const char* PATH_LOG    = "/log";
const char* PATH_WAIT   = "/wait";
const char* PATH_STATUS = "/status";

// Chân điều khiển
#define PIN_MODE_MAY_PHAT 16
#define PIN_MODE_CAN_CAU  17
#define PIN_DATA_1        26
#define PIN_DATA_2        27

const String ID_MAY_PHAT_1 = "MAY_PHAT_1";
const String ID_MAY_PHAT_2 = "MAY_PHAT_2";
const String ID_CAN_CAU_1  = "CAN_CAU_1";
const String ID_CAN_CAU_2  = "CAN_CAU_2";

// Trạng thái & thời gian
bool lastData1State = false, lastData2State = false;
unsigned long data1StartTime = 0, data2StartTime = 0;
unsigned long lastLinkOkTime = 0;

// Buffer file
#define BUFFER_FILE "/buffer.txt"

// NTP
EthernetUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7*3600, 60000);

// SSL Client - PHIÊN BẢN MỚI 2025
EthernetClient base_client;
SSLClient api_client(base_client, TAs, TAs_NUM);

// ====================== HÀM HỖ TRỢ ======================
String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

String getFormattedDateTime(unsigned long epoch) {
  if (epoch == 0) return "1970-01-01 00:00:00";
  time_t rawtime = epoch;
  struct tm *ti = localtime(&rawtime);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
           ti->tm_hour, ti->tm_min, ti->tm_sec);
  return String(buf);
}

String getDeviceId(bool isData1) {
  bool modeMayPhat = (digitalRead(PIN_MODE_MAY_PHAT) == LOW);
  bool modeCanCau  = (digitalRead(PIN_MODE_CAN_CAU) == LOW);

  if (isData1) {
    return modeMayPhat ? ID_MAY_PHAT_1 : (modeCanCau ? ID_CAN_CAU_1 : "");
  } else {
    return modeMayPhat ? ID_MAY_PHAT_2 : (modeCanCau ? ID_CAN_CAU_2 : "");
  }
}

// Đọc dòng HTTP (tương thích SSLClient mới)
String readLine(SSLClient& client, uint32_t timeoutMs = 10000) {
  String line = "";
  unsigned long timeout = millis() + timeoutMs;

  while (millis() < timeout && client.connected()) {
    if (client.available()) {
      char c = client.read();
      if (c == '\r') continue;
      if (c == '\n') {
        break;
      }
      if (line.length() < 1024) line += c;
      else break; // tránh tràn
    } else {
      delay(1);
    }
  }
  return line;
}

// ====================== GỬI DỮ LIỆU ======================
bool sendDataToApi(String timestamp, String status, String uptime, bool isResent, String deviceId) {
  if (Ethernet.linkStatus() != LinkON) return false;

  char json[512];
  snprintf(json, sizeof(json),
    "{\"device_key\":\"%s\",\"device\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\",\"uptime\":\"%s\",\"localip\":\"%s\",\"resent\":%s}",
    DEVICE_KEY.c_str(), deviceId.c_str(), status.c_str(), timestamp.c_str(),
    uptime.c_str(), ipToString(Ethernet.localIP()).c_str(), isResent ? "true" : "false");

  if (!api_client.connect(API_HOST, API_PORT)) return false;

  api_client.print(F("POST ")); api_client.print(PATH_LOG); api_client.println(F(" HTTP/1.1"));
  api_client.print(F("Host: ")); api_client.println(API_HOST);
  api_client.println(F("Content-Type: application/json"));
  api_client.print(F("Content-Length: ")); api_client.println(strlen(json));
  api_client.println(F("Connection: close"));
  api_client.println();
  api_client.print(json);

  String statusLine = readLine(api_client, 12000);
  api_client.stop();

  bool success = statusLine.indexOf("200") != -1 || statusLine.indexOf("201") != -1;
  if (success) lastLinkOkTime = millis();
  return success;
}

bool sendStatusToServer(String timestamp, String status, String uptime, String deviceId) {
  char json[512];
  snprintf(json, sizeof(json),
    "{\"device_key\":\"%s\",\"device\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\",\"uptime\":\"%s\",\"localip\":\"%s\"}",
    DEVICE_KEY.c_str(), deviceId.c_str(), status.c_str(), timestamp.c_str(),
    uptime.c_str(), ipToString(Ethernet.localIP()).c_str());

  if (!api_client.connect(API_HOST, API_PORT)) return false;

  api_client.print(F("POST ")); api_client.print(PATH_STATUS); api_client.println(F(" HTTP/1.1"));
  api_client.print(F("Host: ")); api_client.println(API_HOST);
  api_client.println(F("Content-Type: application/json"));
  api_client.print(F("Content-Length: ")); api_client.println(strlen(json));
  api_client.println(F("Connection: close"));
  api_client.println();
  api_client.print(json);

  String statusLine = readLine(api_client, 10000);
  api_client.stop();
  return statusLine.indexOf("200") != -1;
}

// ====================== LONG POLL ======================
bool waitForCommand(uint32_t timeoutMs = 60000) {
  if (Ethernet.linkStatus() != LinkON) return false;
  if (!api_client.connect(API_HOST, API_PORT)) return false;

  api_client.print(F("GET ")); api_client.print(PATH_WAIT);
  api_client.print(F("?device_key=")); api_client.print(DEVICE_KEY);
  api_client.println(F(" HTTP/1.1"));
  api_client.print(F("Host: ")); api_client.println(API_HOST);
  api_client.println(F("Connection: close"));
  api_client.println();

  unsigned long start = millis();
  while (millis() - start < timeoutMs && api_client.connected()) {
    if (api_client.available()) break;
    delay(10);
  }

  String statusLine = readLine(api_client);
  if (statusLine.length() == 0) { api_client.stop(); return false; } // timeout or no response
  if (!statusLine.startsWith("HTTP/1.1 200")) { api_client.stop(); return false; }

  // Bỏ qua header
  while (api_client.connected()) {
    String h = readLine(api_client);
    if (h.length() == 0) break;
  }

  // Đọc body an toàn
  String body = "";
  unsigned long bodyStart = millis();
  while (millis() - bodyStart < 2000) { // giới hạn thời gian đọc body
    while (api_client.available()) {
      body += (char)api_client.read();
    }
    if (!api_client.connected()) break;
    delay(5);
  }
  api_client.stop();

  return body.indexOf("\"cmd\":\"send\"") != -1;
}

// ====================== BUFFER AN TOÀN ======================
void saveDataToBuffer(String timestamp, String status, String uptime, String deviceId) {
  File file = SPIFFS.open(BUFFER_FILE, FILE_APPEND);
  if (file) {
    file.printf("%s;%s;%s;%s\n", timestamp.c_str(), status.c_str(), uptime.c_str(), deviceId.c_str());
    file.close();
  }
}

void sendBufferedData() {
  if (!SPIFFS.exists(BUFFER_FILE)) return;

  File file = SPIFFS.open(BUFFER_FILE, FILE_READ);
  if (!file) return;

  File tempFile = SPIFFS.open("/temp.txt", FILE_WRITE);
  if (!tempFile) {
    Serial.println(F("[BUFFER] Không tạo được file tạm!"));
    file.close();
    return;
  }

  bool hasUnsent = false;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int s1 = line.indexOf(';');
    int s2 = line.indexOf(';', s1 + 1);
    int s3 = line.indexOf(';', s2 + 1);
    if (s3 == -1) { tempFile.println(line); hasUnsent = true; continue; }

    String ts = line.substring(0, s1);
    String st = line.substring(s1 + 1, s2);
    String up = line.substring(s2 + 1, s3);
    String id = line.substring(s3 + 1);

    if (!sendDataToApi(ts, st, up, true, id)) {
      tempFile.println(line);
      hasUnsent = true;
    }
  }
  file.close();
  tempFile.close();

  if (!hasUnsent) {
    SPIFFS.remove(BUFFER_FILE);
    SPIFFS.remove("/temp.txt"); // xóa file rác nếu có
  } else {
    SPIFFS.remove(BUFFER_FILE);
    SPIFFS.rename("/temp.txt", BUFFER_FILE);
  }
}

// ====================== XỬ LÝ TRẠNG THÁI ======================
void handleStateChange(bool isOn, unsigned long &startTime, const String& deviceId) {
  unsigned long now = timeClient.getEpochTime();
  if (now < 946684800UL) now = millis() / 1000UL + 946684800UL; // fallback nếu giờ sai

  String timestamp = getFormattedDateTime(now);
  String status = isOn ? "Bật" : "Tắt";
  String uptime = "0g0p";

  if (!isOn && startTime > 0) {
    unsigned long secs = now - startTime;
    uptime = String(secs / 3600) + "g" + String((secs % 3600) / 60) + "p";
  }
  if (isOn) startTime = now;

  Serial.println("[" + deviceId + "] " + status + " @ " + timestamp + " | " + uptime);

  if (sendDataToApi(timestamp, status, uptime, false, deviceId)) {
    return;
  }
  saveDataToBuffer(timestamp, status, uptime, deviceId);
}

// ====================== ETHERNET & WATCHDOG ======================
bool ensureEthernet() {
  if (Ethernet.linkStatus() == LinkON) {
    lastLinkOkTime = millis();
    return true;
  }

  Serial.println(F("[ETH] Mất kết nối, đang thử lại..."));
  Ethernet.begin(mac);
  delay(1000);

  if (Ethernet.linkStatus() == LinkON) {
    Serial.print(F("[ETH] Đã kết nối - IP: "));
    Serial.println(Ethernet.localIP());
    lastLinkOkTime = millis();
    return true;
  }
  return false;
}

// ====================== LONG POLL TASK ======================
TaskHandle_t longPollTask;
void longPollLoop(void *pvParameters) {
  for (;;) {
    if (Ethernet.linkStatus() == LinkON && waitForCommand(55000)) {
      unsigned long now = timeClient.getEpochTime();
      if (now < 946684800UL) now = millis() / 1000UL + 946684800UL;

      String ts = getFormattedDateTime(now);
      String contextId = getDeviceId(true); // không quan trọng, chỉ cần 1 cái
      String status = (digitalRead(PIN_DATA_1) == LOW || digitalRead(PIN_DATA_2) == LOW) ? "Bật" : "Tắt";
      sendStatusToServer(ts, status, "0g0p", contextId.length() > 0 ? contextId : "UNKNOWN");
    }
    vTaskDelay(pdMS_TO_TICKS(3000)); // 3 giây/lần kiểm tra
  }
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(500);
  Serial.println(F("\n=== ESP32 IoT UNIT START ==="));
  Serial.println("DEVICE_KEY: " + DEVICE_KEY);

  pinMode(PIN_MODE_MAY_PHAT, INPUT_PULLUP);
  pinMode(PIN_MODE_CAN_CAU, INPUT_PULLUP);
  pinMode(PIN_DATA_1, INPUT_PULLUP);
  pinMode(PIN_DATA_2, INPUT_PULLUP);

  if (!SPIFFS.begin(true)) {
    Serial.println(F("SPIFFS Mount Failed!"));
    return;
  }

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  if (RST_PIN != -1) {
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW); delay(50);
    digitalWrite(RST_PIN, HIGH); delay(200);
  }
  Ethernet.init(CS_PIN);

  ensureEthernet();
  // chờ có mạng
  timeClient.begin();
  for (int i = 0; i < 10 && !timeClient.forceUpdate(); i++) delay(500);

  // CẤU HÌNH SSL ĐÚNG PHIÊN BẢN MỚI
  api_client.setTrustAnchors(TAs, TAs_NUM);
  api_client.setServerName(API_HOST);        // QUAN TRỌNG NHẤT
  api_client.setTimeout(15000);

  // Gửi trạng thái khởi động
  delay(2000);
  unsigned long now = timeClient.getEpochTime();
  if (now > 946684800UL) {
    bool d1 = (digitalRead(PIN_DATA_1) == LOW);
    bool d2 = (digitalRead(PIN_DATA_2) == LOW);
    String id1 = getDeviceId(true);
    String id2 = getDeviceId(false);

    if (d1 && id1 != "") handleStateChange(true, data1StartTime, id1);
    if (d2 && id2 != "") handleStateChange(true, data2StartTime, id2);
  }

  lastData1State = (digitalRead(PIN_DATA_1) == LOW);
  lastData2State = (digitalRead(PIN_DATA_2) == LOW);

  xTaskCreatePinnedToCore(longPollLoop, "LongPoll", 12288, NULL, 1, &longPollTask, 1);

  Serial.println(F("=== KHỞI ĐỘNG THÀNH CÔNG ==="));
}

// ====================== LOOP ======================
void loop() {
  static uint32_t lastBufferCheck = 0;
  static uint32_t lastNtpUpdate = 0;

  // Cập nhật giờ an toàn
  if (millis() - lastNtpUpdate > 300000) { // 5 phút/lần
    if (Ethernet.linkStatus() == LinkON) timeClient.update();
    lastNtpUpdate = millis();
  }

  // Gửi buffer mỗi 10s khi có mạng
  if (millis() - lastBufferCheck > 10000) {
    lastBufferCheck = millis();
    if (Ethernet.linkStatus() == LinkON) sendBufferedData();
  }

  // Watchdog: restart nếu mất mạng quá 5 phút
  if (millis() - lastLinkOkTime > 300000) {
    Serial.println(F("[WATCHDOG] Mất mạng quá lâu → Restart..."));
    ESP.restart();
  }

  ensureEthernet();

  bool isData1On = (digitalRead(PIN_DATA_1) == LOW);
  bool isData2On = (digitalRead(PIN_DATA_2) == LOW);

  String id1 = getDeviceId(true);
  String id2 = getDeviceId(false);

  if (isData1On != lastData1State && id1 != "") {
    handleStateChange(isData1On, data1StartTime, id1);
    lastData1State = isData1On;
  }
  if (isData2On != lastData2State && id2 != "") {
    handleStateChange(isData2On, data2StartTime, id2);
    lastData2State = isData2On;
  }

  delay(100);
}