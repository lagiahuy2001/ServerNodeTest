#include <SPI.h>
#include <EthernetLarge.h>
#include <SSLClient.h>
#include "trust_anchors.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SPIFFS.h>

// ====================== CẤU HÌNH ======================
#define CS_PIN       21
#define RST_PIN      4
#define SPI_SCK_PIN  18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
const String DEVICE_KEY = "ESP_UNIT_001";  // ← THAY ĐỔI CHO TỪNG ESP

const char* API_HOST = "servernodetest-eiq5.onrender.com";
const int   API_PORT = 443;
const char* PATH_LOG   = "/log";
const char* PATH_WAIT  = "/wait";
const char* PATH_STATUS = "/status";

// Chân
#define PIN_MODE_MAY_PHAT 16
#define PIN_MODE_CAN_CAU  17
#define PIN_DATA_1        26
#define PIN_DATA_2        27

const String ID_MAY_PHAT_1 = "MAY_PHAT_1";
const String ID_MAY_PHAT_2 = "MAY_PHAT_2";
const String ID_CAN_CAU_1  = "CAN_CAU_1";
const String ID_CAN_CAU_2  = "CAN_CAU_2";

// Trạng thái
bool lastData1State = false, lastData2State = false;
unsigned long data1StartTime = 0, data2StartTime = 0;

// Buffer
#define BUFFER_FILE "/buffer.txt"

// NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);

// SSL
EthernetClient base_client;
SSLClient api_client(base_client, TAs, TAs_NUM, 0, 2048, (SSLClient::DebugLevel)0);

// ====================== HÀM PHỤ TRỢ ======================
String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

String getFormattedDateTime(unsigned long epoch) {
  time_t rawtime = epoch;
  struct tm *ti = localtime(&rawtime);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
           ti->tm_hour, ti->tm_min, ti->tm_sec);
  return String(buf);
}

String readLine(Stream& client, uint32_t timeoutMs = 10000) {
  String line = "";
  char c;
  uint32_t timeout = millis() + timeoutMs;
  while (millis() < timeout && client.connected()) {
    if (client.available()) {
      c = client.read();
      if (c == '\r') continue;
      if (c == '\n') break;
      if (line.length() < 256) line += c;
    }
    yield();
  }
  return line;
}

// ====================== GỬI DỮ LIỆU ======================
bool sendDataToApi(String timestamp, String status, String uptime, bool isResent, String deviceId) {
  char json[512];
  snprintf(json, sizeof(json),
    "{\"device_key\":\"%s\",\"device\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\",\"uptime\":\"%s\",\"localip\":\"%s\",\"resent\":%s}",
    DEVICE_KEY.c_str(), deviceId.c_str(), status.c_str(), timestamp.c_str(),
    uptime.c_str(), ipToString(Ethernet.localIP()).c_str(), isResent ? "true" : "false");

  if (Ethernet.linkStatus() != LinkON) return false;
  if (!api_client.connect(API_HOST, API_PORT)) return false;

  api_client.print(F("POST ")); api_client.print(PATH_LOG); api_client.println(F(" HTTP/1.1"));
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

bool sendStatusToServer(String timestamp, String status, String uptime, String deviceId) {
  char json[512];
  snprintf(json, sizeof(json),
    "{\"device_key\":\"%s\",\"device\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\",\"uptime\":\"%s\",\"localip\":\"%s\"}",
    DEVICE_KEY.c_str(), deviceId.c_str(), status.c_str(), timestamp.c_str(),
    uptime.c_str(), ipToString(Ethernet.localIP()).c_str());

  if (Ethernet.linkStatus() != LinkON) return false;
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

// ====================== LONG-POLL (RIÊNG BIỆT) ======================
bool waitForCommand(uint32_t timeoutMs = 60000) {
  if (Ethernet.linkStatus() != LinkON) return false;
  if (!api_client.connect(API_HOST, API_PORT)) return false;

  api_client.print(F("GET ")); api_client.print(PATH_WAIT); api_client.print(F("?device_key="));
  api_client.print(DEVICE_KEY); api_client.println(F(" HTTP/1.1"));
  api_client.print(F("Host: ")); api_client.println(API_HOST);
  api_client.println(F("Connection: close"));
  api_client.println();

  uint32_t t0 = millis();
  while (api_client.connected() && !api_client.available()) {
    if (millis() - t0 > timeoutMs) break;
    yield();
  }

  String statusLine = readLine(api_client, 15000);
  if (!statusLine.startsWith("HTTP/1.1 200")) {
    api_client.stop(); return false;
  }

  while (api_client.connected()) {
    String h = readLine(api_client);
    if (h.length() == 0) break;
  }

  String body;
  while (api_client.connected() && api_client.available()) {
    body += (char)api_client.read();
  }
  api_client.stop();

  return body.indexOf("\"cmd\":\"send\"") != -1;
}

// ====================== BUFFER ======================
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

  File temp = SPIFFS.open("/temp.txt", FILE_WRITE);
  if (!temp) { file.close(); return; }

  bool allSent = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int s1 = line.indexOf(';');
    int s2 = line.indexOf(';', s1 + 1);
    int s3 = line.indexOf(';', s2 + 1);
    if (s3 == -1) continue;

    String ts = line.substring(0, s1);
    String st = line.substring(s1 + 1, s2);
    String up = line.substring(s2 + 1, s3);
    String id = line.substring(s3 + 1);

    if (!sendDataToApi(ts, st, up, true, id)) {
      temp.println(line);
      allSent = false;
      break;
    }
  }

  while (file.available()) temp.println(file.readStringUntil('\n'));
  file.close(); temp.close();

  SPIFFS.remove(BUFFER_FILE);
  if (!allSent) SPIFFS.rename("/temp.txt", BUFFER_FILE);
  else SPIFFS.remove("/temp.txt");
}

// ====================== XỬ LÝ TRẠNG THÁI (TỰ ĐỘNG) ======================
void handleStateChange(bool isNowOn, unsigned long &startTime, const String& deviceId) {
  unsigned long now = timeClient.getEpochTime();
  String timestamp = getFormattedDateTime(now);
  String status = isNowOn ? "Bật" : "Tắt";
  String uptime = "0g0p";

  if (!isNowOn && startTime > 0) {
    unsigned long secs = now - startTime;
    uptime = String(secs / 3600) + "g" + String((secs % 3600) / 60) + "p";
  }
  if (isNowOn) startTime = now;

  Serial.println("[" + deviceId + "] " + status + " @ " + timestamp + " | Uptime: " + uptime);

  // GỬI NGAY, KHÔNG CHỜ
  if (sendDataToApi(timestamp, status, uptime, false, deviceId)) {
    return;
  }

  // Nếu thất bại → lưu buffer
  saveDataToBuffer(timestamp, status, uptime, deviceId);
}

// ====================== ETHERNET RECONNECT ======================
bool ensureEthernet() {
  if (Ethernet.linkStatus() == LinkON) return true;

  Serial.println(F("[ETH] Reconnecting..."));
  Ethernet.begin(mac);
  delay(1000);
  if (Ethernet.linkStatus() == LinkON) {
    Serial.print(F("IP: ")); Serial.println(Ethernet.localIP());
    return true;
  }
  return false;
}

// ====================== LONG-POLL TASK ======================
TaskHandle_t longPollTask;
void longPollLoop(void *pvParameters) {
  for (;;) {
    if (ensureEthernet() && waitForCommand()) {
      // Lệnh từ server → gửi trạng thái hiện tại
      unsigned long now = timeClient.getEpochTime();
      String ts = getFormattedDateTime(now);

      bool modeMayPhat = (digitalRead(PIN_MODE_MAY_PHAT) == LOW);
      bool modeCanCau  = (digitalRead(PIN_MODE_CAN_CAU) == LOW);
      String contextId = "UNKNOWN";

      if (modeMayPhat) {
        contextId = (digitalRead(PIN_DATA_1) == LOW) ? ID_MAY_PHAT_1 : ID_MAY_PHAT_2;
      } else if (modeCanCau) {
        contextId = (digitalRead(PIN_DATA_1) == LOW) ? ID_CAN_CAU_1 : ID_CAN_CAU_2;
      }

      String status = (digitalRead(PIN_DATA_1) == LOW || digitalRead(PIN_DATA_2) == LOW) ? "Bật" : "Tắt";
      sendStatusToServer(ts, status, "0g0p", contextId);
    }
    delay(1000);  // Giữ kết nối ổn định
  }
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200); while (!Serial); delay(500);
  Serial.println(F("\n=== ESP32 IoT Unit ==="));
  Serial.println("DEVICE_KEY: " + DEVICE_KEY);

  pinMode(PIN_MODE_MAY_PHAT, INPUT_PULLUP);
  pinMode(PIN_MODE_CAN_CAU, INPUT_PULLUP);
  pinMode(PIN_DATA_1, INPUT_PULLUP);
  pinMode(PIN_DATA_2, INPUT_PULLUP);

  if (!SPIFFS.begin(true)) { Serial.println(F("SPIFFS Failed")); return; }

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  if (RST_PIN != -1) { pinMode(RST_PIN, OUTPUT); digitalWrite(RST_PIN, LOW); delay(50); digitalWrite(RST_PIN, HIGH); delay(200); }
  Ethernet.init(CS_PIN);

  ensureEthernet();
  timeClient.begin();
  while (!timeClient.update()) { timeClient.forceUpdate(); delay(500); }

  SSLClient::SSLConfig cfg; cfg.sni_hostname = API_HOST;
  api_client.setSSLConfig(cfg); api_client.setTimeout(10000);

  lastData1State = (digitalRead(PIN_DATA_1) == LOW);
  lastData2State = (digitalRead(PIN_DATA_2) == LOW);

  xTaskCreatePinnedToCore(longPollLoop, "LongPoll", 8192, NULL, 1, &longPollTask, 1);
}

// ====================== LOOP ======================
void loop() {
  static uint32_t lastBufferCheck = 0;
  timeClient.update();

  if (millis() - lastBufferCheck > 10000) {
    lastBufferCheck = millis();
    if (Ethernet.linkStatus() == LinkON) sendBufferedData();
  }

  ensureEthernet();

  bool modeMayPhat = (digitalRead(PIN_MODE_MAY_PHAT) == LOW);
  bool modeCanCau  = (digitalRead(PIN_MODE_CAN_CAU) == LOW);
  bool isData1On = (digitalRead(PIN_DATA_1) == LOW);
  bool isData2On = (digitalRead(PIN_DATA_2) == LOW);

  String id1 = modeMayPhat ? ID_MAY_PHAT_1 : (modeCanCau ? ID_CAN_CAU_1 : "");
  String id2 = modeMayPhat ? ID_MAY_PHAT_2 : (modeCanCau ? ID_CAN_CAU_2 : "");

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