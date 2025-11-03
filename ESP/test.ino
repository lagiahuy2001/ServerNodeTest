#include <SPI.h>
#include <EthernetLarge.h>   // Dùng bản Large để buffer lớn hơn
#include <SSLClient.h>
#include "trust_anchors.h"   // File chứa Trust Anchors (Let's Encrypt)

// ==== CẤU HÌNH CHÂN SPI (tuỳ chỉnh theo board) ====
#define CS_PIN       21
#define RST_PIN      4
#define SPI_SCK_PIN  18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

// ==== THÔNG TIN DỊCH VỤ ====
const char* HOST = "servernodetest-eiq5.onrender.com";
const int   HTTPS_PORT = 443;
const char* PATH = "/log";

// ==== ETHERNET + SSL CLIENT ====
EthernetClient base_client;
SSLClient ssl_client(base_client, TAs, TAs_NUM);

// ==== MAC address (tuỳ chọn, thay đổi nếu cần) ====
byte MAC[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// ======= HÀM PHỤ TRỢ =======

// In trạng thái liên kết Ethernet
void printLinkStatus() {
  #ifdef ETHERNET_LARGE_H
  EthernetLinkStatus link = Ethernet.linkStatus();
  Serial.print(F("[ETH] Link: "));
  switch (link) {
    case Unknown:  Serial.println(F("Unknown")); break;
    case LinkON:   Serial.println(F("ON")); break;
    case LinkOFF:  Serial.println(F("OFF")); break;
  }
  #endif
}

// In thông tin mạng
void printNetworkInfo() {
  IPAddress ip = Ethernet.localIP();
  Serial.print(F("[ETH] IP: "));       Serial.println(ip);
  Serial.print(F("[ETH] Subnet: "));   Serial.println(Ethernet.subnetMask());
  Serial.print(F("[ETH] Gateway: "));  Serial.println(Ethernet.gatewayIP());
  Serial.print(F("[ETH] DNS: "));      Serial.println(Ethernet.dnsServerIP());
}

// Chuyển IPAddress thành String (an toàn, không dùng toString())
String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

// Đọc 1 dòng từ Stream (an toàn, tránh String lớn)
String readLine(Stream& client) {
  String line = "";
  char c;
  uint32_t timeout = millis() + 5000;
  while (millis() < timeout) {
    if (client.available()) {
      c = client.read();
      if (c == '\r') continue;           // Bỏ \r
      if (c == '\n') break;              // Kết thúc dòng
      if (line.length() < 256) line += c; // Giới hạn độ dài
    }
    yield();
  }
  return line;
}

// Gửi GET request HTTPS và in phản hồi
bool httpsGET_LogOnce() {
  String device    = "ESP32-ETH";
  String status    = "online";
  uint32_t timestamp = millis() / 1000;
  uint32_t uptime    = millis() / 1000;
  String localip   = ipToString(Ethernet.localIP());

  // Tạo URL an toàn với snprintf (tránh String concatenation)
  char url[256];
  snprintf(url, sizeof(url),
           "%s?device=%s&status=%s&timestamp=%lu&uptime=%lu&localip=%s",
           PATH, device.c_str(), status.c_str(), timestamp, uptime, localip.c_str());

  Serial.print(F("[TLS] Connecting to ")); Serial.print(HOST); Serial.print(F(":")); Serial.println(HTTPS_PORT);

  if (!ssl_client.connect(HOST, HTTPS_PORT)) {
    Serial.println(F("[TLS] Connect failed"));
    return false;
  }
  Serial.println(F("[TLS] Connected. Sending request..."));

  // Gửi HTTP request
  ssl_client.print(F("GET "));
  ssl_client.print(url);
  ssl_client.println(F(" HTTP/1.1"));
  ssl_client.print(F("Host: ")); ssl_client.println(HOST);
  ssl_client.println(F("User-Agent: esp32-ethernet/1.0"));
  ssl_client.println(F("Connection: close"));
  ssl_client.println();

  // Chờ phản hồi
  uint32_t t0 = millis();
  while (ssl_client.connected() && !ssl_client.available() && (millis() - t0 < 8000)) {
    yield();
  }
  if (!ssl_client.available()) {
    Serial.println(F("[HTTP] No response (timeout)"));
    ssl_client.stop();
    return false;
  }

  // Đọc status line
  String statusLine = readLine(ssl_client);
  statusLine.trim();
  Serial.print(F("[HTTP] Status: ")); Serial.println(statusLine);

  // Đọc header + body
  bool isHeader = true;
  int contentCount = 0;
  Serial.println(F("[HTTP] --- BODY ---"));

  while (ssl_client.connected() && ssl_client.available()) {
    String line = readLine(ssl_client);
    if (isHeader) {
      if (line.length() == 0) {
        isHeader = false;
        continue;
      }
      // In một số header quan trọng
      if (line.startsWith("Content-Type:") || line.startsWith("Content-Length:")) {
        Serial.println("[HTTP] " + line);
      }
    } else {
      // In body (giới hạn 512 byte)
      if (contentCount < 512) {
        Serial.print(line);
        if (!line.endsWith("\n")) Serial.println();
        contentCount += line.length() + 1;
      } else if (contentCount == 512) {
        Serial.println(F("\n[HTTP] (truncated)"));
        contentCount++;
      }
    }
    yield();
  }

  ssl_client.stop();
  Serial.println(F("\n[TLS] Connection closed"));
  return true;
}

// ======= SETUP =======
void setup() {
  Serial.begin(115200);
  while (!Serial); delay(500);
  Serial.println(F("\n=== ESP32 Ethernet -> Render.com /log test ==="));

  // Khởi tạo SPI thủ công
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);

  // Reset W5500
  if (RST_PIN != -1) {
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW);
    delay(50);
    digitalWrite(RST_PIN, HIGH);
    delay(200);
  }

  // Khởi động Ethernet
  Ethernet.init(CS_PIN);
  Serial.println(F("[ETH] Starting DHCP..."));

  if (Ethernet.begin(MAC) == 0) {
    Serial.println(F("[ETH] DHCP failed! Using static IP..."));
    IPAddress ip(192, 168, 1, 177);
    IPAddress dns(8, 8, 8, 8);
    IPAddress gw(192, 168, 1, 1);
    IPAddress sn(255, 255, 255, 0);
    Ethernet.begin(MAC, ip, dns, gw, sn);
  } else {
    Serial.println(F("[ETH] DHCP OK"));
  }

  printLinkStatus();
  printNetworkInfo();

  // Test 1 lần
  delay(2000);
  bool ok = httpsGET_LogOnce();
  Serial.print(F("[RESULT] /log call: "));
  Serial.println(ok ? F("SUCCESS") : F("FAILED"));
}

void loop() {
  // Không làm gì trong loop
}