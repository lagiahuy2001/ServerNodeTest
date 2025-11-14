#include <SPI.h>
#include <EthernetLarge.h>
#include <SSLClient.h>
#include "trust_anchors.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SPIFFS.h>

// ====================== CẤU HÌNH CHUNG ======================
#define CS_PIN       21
#define RST_PIN      4
#define SPI_SCK_PIN  18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// ==== THIẾT BỊ RIÊNG (THAY ĐỔI KHI UPLOAD CHO TỪNG ESP) ====
const String DEVICE_KEY = "ESP_UNIT_001";  // ← THAY ĐỔI CHO TỪNG ESP

// ==== SERVER CONFIG ====
const char* API_HOST = "servernodetest-eiq5.onrender.com";
const int   API_PORT = 443;
const char* PATH_LOG   = "/log";
const char* PATH_WAIT  = "/wait";
const char* PATH_STATUS = "/status";

// ==== CHÂN TÍN HIỆU ====
#define PIN_MODE_MAY_PHAT 16
#define PIN_MODE_CAN_CAU  17
#define PIN_DATA_1        26
#define PIN_DATA_2        27

const String ID_MAY_PHAT_1 = "MAY_PHAT_1";
const String ID_MAY_PHAT_2 = "MAY_PHAT_2";
const String ID_CAN_CAU_1  = "CAN_CAU_1";
const String ID_CAN_CAU_2  = "CAN_CAU_2";

// ==== BIẾN TRẠNG THÁI ====
bool isData1On = false, lastData1State = false;
bool isData2On = false, lastData2State = false;
unsigned long data1StartTime = 0, data2StartTime = 0;

// ==== BUFFER ====
#define BUFFER_FILE "/buffer.txt"

// ==== NTP ====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);  // GMT+7

// ==== SSL CLIENT ====
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

// ====================== GỬI DỮ LIỆU (LƯU VÀO FILE) ======================
bool sendDataToApi(String timestamp, String status, String uptime, bool isResent, String deviceId) {
  char json[512];
  snprintf(json, sizeof(json),
    "{\"device_key\":\"%s\",\"device\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\",\"uptime\":\"%s\",\"localip\":\"%s\",\"resent\":%s}",
    DEVICE_KEY.c_str(), deviceId.c_str(), status.c_str(), timestamp.c_str(),
    uptime.c_str(), ipToString(Ethernet.localIP()).c_str(), isResent ? "true" : "false");

  if (Ethernet.linkStatus() != LinkON) return false;

  if (!api_client.connect(API_HOST, API_PORT)) {
    Serial.println(F("[TLS] Connect failed (log)"));
    return false;
  }

  api_client.print(F("POST "));
  api_client.print(PATH_LOG);
  api_client.println(F(" HTTP/1.1"));
  api_client.print(F("Host: ")); api_client.println(API_HOST);
  api_client.println(F("Content-Type: application/json"));
  api_client.print(F("Content-Length: ")); api_client.println(strlen(json));
  api_client.println(F("Connection: close"));
  api_client.println();
  api_client.print(json);

  String statusLine = readLine(api_client, 10000);
  api_client.stop();

  bool ok = statusLine.indexOf("200") != -1;
  Serial.print(F("[LOG] Send ")); Serial.println(ok ? F("OK") : F("FAIL"));
  return ok;
}

// ====================== GỬI TRẠNG THÁI THEO YÊU CẦU (CHỈ POPUP) ======================
bool sendStatusToServer(String timestamp, String status, String uptime, String deviceId) {
  char json[512];
  snprintf(json, sizeof(json),
    "{\"device_key\":\"%s\",\"device\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\",\"uptime\":\"%s\",\"localip\":\"%s\"}",
    DEVICE_KEY.c_str(), deviceId.c_str(), status.c_str(), timestamp.c_str(),
    uptime.c_str(), ipToString(Ethernet.localIP()).c_str());

  if (Ethernet.linkStatus() != LinkON) return false;

  if (!api_client.connect(API_HOST, API_PORT)) {
    Serial.println(F("[TLS] Connect failed (status)"));
    return false;
  }

  api_client.print(F("POST "));
  api_client.print(PATH_STATUS);
  api_client.println(F(" HTTP/1.1"));
  api_client.print(F("Host: ")); api_client.println(API_HOST);
  api_client.println(F("Content-Type: application/json"));
  api_client.print(F("Content-Length: ")); api_client.println(strlen(json));
  api_client.println(F("Connection: close"));
  api_client.println();
  api_client.print(json);

  String statusLine = readLine(api_client, 10000);
  api_client.stop();

  bool ok = statusLine.indexOf("200") != -1;
  Serial.print(F("[STATUS] Send ")); Serial.println(ok ? F("OK") : F("FAIL"));
  return ok;
}

// ====================== LONG-POLL LẮNG NGHE ======================
bool waitForCommand(uint32_t timeoutMs = 60000) {
  if (Ethernet.linkStatus() != LinkON) {
    Serial.println(F("[ETH] Link down → skip wait"));
    delay(1000);
    return false;
  }

  if (!api_client.connect(API_HOST, API_PORT)) {
    Serial.println(F("[TLS] Connect failed (wait)"));
    return false;
  }

  api_client.print(F("GET "));
  api_client.print(PATH_WAIT);
  api_client.print(F("?device_key="));
  api_client.print(DEVICE_KEY);
  api_client.println(F(" HTTP/1.1"));
  api_client.print(F("Host: ")); api_client.println(API_HOST);
  api_client.println(F("Connection: close"));
  api_client.println();

  uint32_t t0 = millis();
  while (api_client.connected() && !api_client.available()) {
    if (millis() - t0 > timeoutMs) {
      Serial.println(F("[WAIT] Timeout"));
      api_client.stop();
      return false;
    }
    yield();
  }

  String statusLine = readLine(api_client, 15000);
  if (!statusLine.startsWith("HTTP/1.1 200")) {
    Serial.print(F("[WAIT] Bad status: ")); Serial.println(statusLine);
    api_client.stop();
    return false;
  }

  // Bỏ qua header
  while (api_client.connected()) {
    String h = readLine(api_client);
    if (h.length() == 0) break;
  }

  String body;
  while (api_client.connected() && api_client.available()) {
    body += (char)api_client.read();
  }
  api_client.stop();

  Serial.print(F("[WAIT] Response: ")); Serial.println(body);

  // Nếu server ra lệnh send → gửi trạng thái hiện tại (không lưu)
  if (body.indexOf("\"cmd\":\"send\"") != -1) {
    unsigned long now = timeClient.getEpochTime();
    String ts = getFormattedDateTime(now);

    // Xác định trạng thái hiện tại
    bool modeMayPhat = (digitalRead(PIN_MODE_MAY_PHAT) == LOW);
    bool modeCanCau  = (digitalRead(PIN_MODE_CAN_CAU) == LOW);
    String contextId = "";

    if (modeMayPhat) {
      contextId = (digitalRead(PIN_DATA_1) == LOW) ? ID_MAY_PHAT_1 : ID_MAY_PHAT_2;
    } else if (modeCanCau) {
      contextId = (digitalRead(PIN_DATA_1) == LOW) ? ID_CAN_CAU_1 : ID_CAN_CAU_2;
    } else {
      contextId = "UNKNOWN";
    }

    String status = (digitalRead(PIN_DATA_1) == LOW || digitalRead(PIN_DATA_2) == LOW) ? "Bật" : "Tắt";
    String uptime = "0g0p";  // Có thể tính nếu cần

    sendStatusToServer(ts, status, uptime, contextId);
    return true;
  }

  return false;
}

// ====================== BUFFER ======================
void saveDataToBuffer(String timestamp, String status, String uptime, String deviceId) {
  File file = SPIFFS.open(BUFFER_FILE, FILE_APPEND);
  if (!file) {
    Serial.println(F("Failed to open buffer file"));
    return;
  }
  String line = timestamp + ";" + status + ";" + uptime + ";" + deviceId + "\n";
  file.print(line);
  file.close();
  Serial.println(F("Saved to buffer: ") + line);
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
  if (!allSent) {
    SPIFFS.rename("/temp.txt", BUFFER_FILE);
  } else {
    SPIFFS.remove("/temp.txt");
  }
}

// ====================== XỬ LÝ TRẠNG THÁI (TỰ ĐỘNG) ======================
void handleStateChange(bool isNowOn, unsigned long &startTime, const String& deviceId) {
  unsigned long currentTime = timeClient.getEpochTime();
  String timestamp = getFormattedDateTime(currentTime);
  String status = isNowOn ? "Bật" : "Tắt";
  String uptime = "0g0p";

  if (isNowOn) {
    startTime = currentTime;
  } else if (startTime > 0) {
    unsigned long secs = currentTime - startTime;
    uptime = String(secs / 3600) + "g" + String((secs % 3600) / 60) + "p";
  }

  Serial.println("[" + deviceId + "] " + status + " @ " + timestamp + " | Uptime: " + uptime);

  // Chờ lệnh từ server
  if (waitForCommand()) {
    Serial.println(F("[CMD] Server requested → sent via /status"));
    return;
  }

  // Lưu buffer nếu không gửi được
  saveDataToBuffer(timestamp, status, uptime, deviceId);
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  while (!Serial); delay(500);
  Serial.println(F("\n=== ESP32 IoT Unit ==="));
  Serial.println("DEVICE_KEY: " + DEVICE_KEY);

  // Khởi tạo chân
  pinMode(PIN_MODE_MAY_PHAT, INPUT_PULLUP);
  pinMode(PIN_MODE_CAN_CAU, INPUT_PULLUP);
  pinMode(PIN_DATA_1, INPUT_PULLUP);
  pinMode(PIN_DATA_2, INPUT_PULLUP);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println(F("SPIFFS Mount Failed"));
    return;
  }

  // Ethernet
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  if (RST_PIN != -1) {
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW); delay(50);
    digitalWrite(RST_PIN, HIGH); delay(200);
  }
  Ethernet.init(CS_PIN);

  Serial.println(F("[ETH] Starting..."));
  if (Ethernet.begin(mac) == 0) {
    Serial.println(F("DHCP failed → static IP"));
    IPAddress ip(192, 168, 1, 177);
    IPAddress dns(8, 8, 8, 8);
    IPAddress gw(192, 168, 1, 1);
    IPAddress sn(255, 255, 255, 0);
    Ethernet.begin(mac, ip, dns, gw, sn);
  }
  Serial.print(F("IP: ")); Serial.println(Ethernet.localIP());

  // NTP
  timeClient.begin();
  while (!timeClient.update()) {
    timeClient.forceUpdate();
    delay(500);
  }
  Serial.println(F("NTP OK"));

  // SSL
  SSLClient::SSLConfig cfg;
  cfg.sni_hostname = API_HOST;
  api_client.setSSLConfig(cfg);
  api_client.setTimeout(10000);

  // Khởi tạo trạng thái ban đầu
  lastData1State = (digitalRead(PIN_DATA_1) == LOW);
  lastData2State = (digitalRead(PIN_DATA_2) == LOW);
  unsigned long now = timeClient.getEpochTime();
  if (digitalRead(PIN_MODE_MAY_PHAT) == LOW && lastData1State) data1StartTime = now;
  if (digitalRead(PIN_MODE_MAY_PHAT) == LOW && lastData2State) data2StartTime = now;
  if (digitalRead(PIN_MODE_CAN_CAU) == LOW && lastData1State) data1StartTime = now;
  if (digitalRead(PIN_MODE_CAN_CAU) == LOW && lastData2State) data2StartTime = now;
}

// ====================== LOOP ======================
void loop() {
  static uint32_t lastBufferCheck = 0;
  timeClient.update();

  // Gửi buffer mỗi 10s
  if (millis() - lastBufferCheck > 10000) {
    lastBufferCheck = millis();
    if (Ethernet.linkStatus() == LinkON) {
      sendBufferedData();
    }
  }

  // Đọc tín hiệu
  bool modeMayPhat = (digitalRead(PIN_MODE_MAY_PHAT) == LOW);
  bool modeCanCau  = (digitalRead(PIN_MODE_CAN_CAU) == LOW);
  isData1On = (digitalRead(PIN_DATA_1) == LOW);
  isData2On = (digitalRead(PIN_DATA_2) == LOW);

  String id1 = "", id2 = "";
  if (modeMayPhat) {
    id1 = ID_MAY_PHAT_1; id2 = ID_MAY_PHAT_2;
  } else if (modeCanCau) {
    id1 = ID_CAN_CAU_1;  id2 = ID_CAN_CAU_2;
  }

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