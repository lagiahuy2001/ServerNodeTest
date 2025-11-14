#include <SPI.h>
#include <EthernetLarge.h>
#include <SSLClient.h>
#include "trust_anchors.h"

#define CS_PIN       21
#define RST_PIN      4
#define SPI_SCK_PIN  18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

const char* HOST       = "servernodetest-eiq5.onrender.com";
const int   HTTPS_PORT = 443;
const char* PATH_LOG   = "/log";
const char* PATH_WAIT  = "/wait";

EthernetClient base_client;
SSLClient ssl_client(base_client, TAs, TAs_NUM, 0, 2048, (SSLClient::DebugLevel)0);

byte MAC[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

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

void printNetworkInfo() {
  IPAddress ip = Ethernet.localIP();
  Serial.print(F("[ETH] IP: "));       Serial.println(ip);
  Serial.print(F("[ETH] Subnet: "));   Serial.println(Ethernet.subnetMask());
  Serial.print(F("[ETH] Gateway: "));  Serial.println(Ethernet.gatewayIP());
  Serial.print(F("[ETH] DNS: "));      Serial.println(Ethernet.dnsServerIP());
}

String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

String readLine(Stream& client, uint32_t timeoutMs = 10000) {
  String line = "";
  char c;
  uint32_t timeout = millis() + timeoutMs;
  while (millis() < timeout) {
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

bool sendLogData() {
  if (Ethernet.linkStatus() != LinkON) {
    Serial.println(F("[ETH] Link down → skip send"));
    return false;
  }

  String device = "ESP32-ETH";
  String status = "online";
  uint32_t ts   = millis() / 1000;
  uint32_t up   = millis() / 1000;
  String ip     = ipToString(Ethernet.localIP());

  char json[256];
  snprintf(json, sizeof(json),
           "{\"device\":\"%s\",\"status\":\"%s\",\"timestamp\":%lu,\"uptime\":%lu,\"localip\":\"%s\"}",
           device.c_str(), status.c_str(), ts, up, ip.c_str());

  if (!ssl_client.connect(HOST, HTTPS_PORT)) {
    Serial.println(F("[TLS] Connect failed (log)"));
    return false;
  }

  ssl_client.print(F("POST "));
  ssl_client.print(PATH_LOG);
  ssl_client.println(F(" HTTP/1.1"));
  ssl_client.print(F("Host: ")); ssl_client.println(HOST);
  ssl_client.println(F("Content-Type: application/json"));
  ssl_client.print(F("Content-Length: ")); ssl_client.println(strlen(json));
  ssl_client.println(F("Connection: close"));
  ssl_client.println();
  ssl_client.print(json);

  uint32_t t0 = millis();
  while (ssl_client.connected() && !ssl_client.available() && (millis() - t0 < 8000)) yield();

  String statusLine = readLine(ssl_client, 10000);
  ssl_client.stop();

  bool ok = statusLine.indexOf("200") != -1;
  Serial.print(F("[LOG] Send ")); Serial.println(ok ? F("OK") : F("FAIL"));
  return ok;
}

bool waitForCommand(uint32_t timeoutMs = 60000) {
  if (Ethernet.linkStatus() != LinkON) {
    Serial.println(F("[ETH] Link down → skip wait"));
    delay(1000);
    return false;
  }

  if (!ssl_client.connect(HOST, HTTPS_PORT)) {
    Serial.println(F("[TLS] Connect failed (wait)"));
    return false;
  }

  ssl_client.print(F("GET "));
  ssl_client.print(PATH_WAIT);
  ssl_client.println(F(" HTTP/1.1"));
  ssl_client.print(F("Host: ")); ssl_client.println(HOST);
  ssl_client.println(F("Connection: close"));
  ssl_client.println();

  uint32_t t0 = millis();
  while (ssl_client.connected() && !ssl_client.available()) {
    if (millis() - t0 > timeoutMs) {
      Serial.println(F("[WAIT] Timeout"));
      ssl_client.stop();
      return false;
    }
    yield();
  }

  String statusLine = readLine(ssl_client, 15000);
  if (!statusLine.startsWith("HTTP/1.1 200")) {
    Serial.print(F("[WAIT] Bad status: ")); Serial.println(statusLine);
    ssl_client.stop();
    return false;
  }

  while (ssl_client.connected()) {
    String h = readLine(ssl_client);
    if (h.length() == 0) break;
  }

  String body;
  while (ssl_client.connected() && ssl_client.available()) {
    body += (char)ssl_client.read();
  }
  ssl_client.stop();

  Serial.print(F("[WAIT] Response: ")); Serial.println(body);
  return (body.indexOf("\"cmd\":\"send\"") != -1);
}

void setup() {
  Serial.begin(115200);
  while (!Serial); delay(500);
  Serial.println(F("\n=== ESP32 Ethernet Long-poll IoT ==="));

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);

  if (RST_PIN != -1) {
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW);
    delay(50);
    digitalWrite(RST_PIN, HIGH);
    delay(200);
  }

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

  Serial.println(F("[SSL] Ready with Let's Encrypt TAs"));
  delay(2000);
}

void loop() {
  static uint32_t lastLinkCheck = 0;
  if (millis() - lastLinkCheck > 10000) {
    lastLinkCheck = millis();
    if (Ethernet.linkStatus() == LinkOFF) {
      Serial.println(F("[ETH] Link lost → restart Ethernet"));
      Ethernet.begin(MAC);
    }
  }

  Serial.println(F("\n[WAIT] Waiting for server command..."));
  if (waitForCommand()) {
    Serial.println(F("[CMD] Server requested data → sending"));
    sendLogData();
  } else {
    Serial.println(F("[CMD] No command (timeout/none)"));
  }

  delay(100);
}