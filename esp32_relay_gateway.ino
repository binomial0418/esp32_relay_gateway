#include "config.h"
#include <DNSServer.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// --- 全域變數 ---
WebSocketsServer webSocket = WebSocketsServer(81);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
DNSServer dnsServer;
WebServer httpServer(80);
WiFiServer rawServer443(443);  // TCP 透明代理，依 SNI 白名單決定是否轉發
WiFiUDP udpQuic80;             // 攔截 QUIC over UDP port 80
WiFiUDP udpQuic443;            // 攔截 QUIC over UDP port 443

struct ProxySession {
  WiFiClient inbound;   // 來自手機
  WiFiClient outbound;  // 至真實伺服器（透過 STA）
  bool active;
};
ProxySession proxySessions[3];

unsigned long lastMqttRetry = 0;
const unsigned long mqttRetryInterval = 5000;

// ================================================================
// HTTP 回應輔助函式
// ================================================================

void send204(WebServer &server) {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Connection", "close");
  server.setContentLength(0);
  server.send(204, "text/plain", "");
}

void sendJson(WebServer &server, const String &json) {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", json);
}

// ================================================================
// HTTP Server (port 80) Handlers
// ================================================================

void handle204() {
  Serial.printf("[REQUEST] HTTP 來自 %s 請求: %s\n",
                httpServer.client().remoteIP().toString().c_str(),
                httpServer.uri().c_str());
  send204(httpServer);
}

void handleRoot() {
  Serial.printf("[REQUEST] HTTP 來自 %s 請求: /\n",
                httpServer.client().remoteIP().toString().c_str());
  httpServer.sendHeader("Cache-Control", "no-cache");
  httpServer.sendHeader("Connection", "close");
  httpServer.send(200, "text/plain", "OK");
}

void handleGetDNList() {
  Serial.printf("[REQUEST] HTTP 來自 %s 請求: %s\n",
                httpServer.client().remoteIP().toString().c_str(),
                httpServer.uri().c_str());
  String json =
      R"({"code":200,"msg":"success","dns":[{"ip":"192.168.4.1","ttl":300},{"ip":"192.168.4.1","ttl":300}]})";
  sendJson(httpServer, json);
}

void handleGetHttpDnsServerList() {
  Serial.printf("[REQUEST] HTTP 來自 %s 請求: %s\n",
                httpServer.client().remoteIP().toString().c_str(),
                httpServer.uri().c_str());
  String json =
      R"({"code":200,"msg":"success","server_list":[{"host":"192.168.4.1","port":80,"ttl":300}]})";
  sendJson(httpServer, json);
}

void handleHttpDnsQuery() {
  String dn = httpServer.arg("dn");
  Serial.printf("[HttpDNS] 來自 %s 查詢域名: %s\n",
                httpServer.client().remoteIP().toString().c_str(), dn.c_str());
  String json = R"({"dns":[{"ip":["192.168.4.1"],"ttl":300}],"code":200})";
  sendJson(httpServer, json);
}

void handleConnectTest() {
  Serial.printf("[REQUEST] HTTP 來自 %s 請求: %s\n",
                httpServer.client().remoteIP().toString().c_str(),
                httpServer.uri().c_str());
  httpServer.sendHeader("Cache-Control", "no-cache");
  httpServer.sendHeader("Connection", "close");
  httpServer.send(200, "text/plain", "Microsoft Connect Test");
}

void handleNotFound() {
  Serial.printf("[UNKNOWN] 未知路徑來自 %s : %s\n",
                httpServer.client().remoteIP().toString().c_str(),
                httpServer.uri().c_str());
  send204(httpServer);
}

// ================================================================
// MQTT
// ================================================================

void reconnectMqtt() {
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > mqttRetryInterval) {
      lastMqttRetry = now;
      Serial.println(F("[MQTT] 嘗試連接至 Broker..."));
      String clientId = "ESP32Relay-";
      clientId += String(random(0xffff), HEX);
      if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
        Serial.println(F("[MQTT] 已連接"));
      } else {
        Serial.print(F("[MQTT] 連接失敗, rc="));
        Serial.print(mqttClient.state());
        Serial.println(F(" 將繼續運作區域網路模式"));
      }
    }
  }
}

// ================================================================
// WebSocket
// ================================================================

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload,
                    size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.printf("[%u] WebSocket 已斷開\n", num);
    break;
  case WStype_CONNECTED: {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[%u] WebSocket 已連接，來自: %d.%d.%d.%d\n", num, ip[0],
                  ip[1], ip[2], ip[3]);
  } break;
  case WStype_TEXT: {
    String message = String((char *)payload);
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[WS] [%u] 來自 %s 收到: %s\n", num, ip.toString().c_str(),
                  message.c_str());
    if (mqttClient.connected()) {
      if (mqttClient.publish(mqtt_topic, message.c_str())) {
        Serial.println(F("[轉發] MQTT 發送成功"));
      } else {
        Serial.println(F("[轉發] MQTT 發送失敗"));
      }
    } else {
      Serial.println(F("[轉發] MQTT 未連接，數據已丟棄"));
    }
  } break;
  default:
    break;
  }
}

// ================================================================
// TLS SNI 解析（從 ClientHello 取出目標 hostname）
// ================================================================

String parseSNI(const uint8_t *buf, int len) {
  if (len < 44)       return "";
  if (buf[0] != 0x16) return "";  // Content Type: Handshake
  if (buf[5] != 0x01) return "";  // Handshake Type: ClientHello

  int pos = 43;
  if (pos >= len) return "";

  // 跳過 Session ID
  int sidLen = buf[pos++];
  pos += sidLen;
  if (pos + 2 > len) return "";

  // 跳過 Cipher Suites
  int csLen = (int(buf[pos]) << 8) | buf[pos + 1];
  pos += 2 + csLen;
  if (pos + 1 > len) return "";

  // 跳過 Compression Methods
  int cmLen = buf[pos++];
  pos += cmLen;
  if (pos + 2 > len) return "";

  // 掃描 Extensions
  int extEnd = pos + 2 + ((int(buf[pos]) << 8) | buf[pos + 1]);
  pos += 2;

  while (pos + 4 <= extEnd && pos + 4 <= len) {
    int extType    = (int(buf[pos]) << 8) | buf[pos + 1];
    int extDataLen = (int(buf[pos + 2]) << 8) | buf[pos + 3];
    pos += 4;

    if (extType == 0x0000 && pos + 5 <= len) {  // SNI extension
      pos += 2;  // SNI list length
      pos += 1;  // name type
      int nameLen = (int(buf[pos]) << 8) | buf[pos + 1];
      pos += 2;
      if (pos + nameLen <= len)
        return String((const char *)&buf[pos], nameLen);
      return "";
    }
    pos += extDataLen;
  }
  return "";
}

// ================================================================
// HTTPS TCP 透明代理 (port 443)
// 白名單：連線偵測 / 連線品質 / OPPO 相關網域
// ================================================================

bool isProxyHost(const String &host) {
  String h = host;
  h.toLowerCase();
  return h.indexOf("connectivitycheck") >= 0 ||
         h.indexOf("clients3.google")   >= 0 ||
         h.indexOf("captive.apple")     >= 0 ||
         h.indexOf("heytapmobile")      >= 0 ||
         h.indexOf("allawnos.com")      >= 0 ||  // OPPO ColorOS connectivity check
         h.indexOf("oppo.com")          >= 0 ||
         h.indexOf("android.apis.google.com") >= 0 ||  // Android GMS 裝置驗證
         h.indexOf("www.google.us")     >= 0;   // Google 連線品質偵測
}

void handleTcpProxy443() {
  // 1. 中繼現有 session 的資料
  for (int i = 0; i < 3; i++) {
    ProxySession &s = proxySessions[i];
    if (!s.active) continue;

    uint8_t buf[512];
    int n;

    while (s.inbound.available() && s.outbound.connected()) {
      n = s.inbound.read(buf, sizeof(buf));
      if (n > 0) s.outbound.write(buf, n);
    }
    while (s.outbound.available() && s.inbound.connected()) {
      n = s.outbound.read(buf, sizeof(buf));
      if (n > 0) s.inbound.write(buf, n);
    }

    if (!s.inbound.connected() || !s.outbound.connected()) {
      s.inbound.stop();
      s.outbound.stop();
      s.active = false;
      Serial.println(F("[TLS] 代理連線已關閉"));
    }
  }

  // 2. 接受新連線
  WiFiClient newClient = rawServer443.available();
  if (!newClient) return;

  // 累積 ClientHello 直到 SNI 解析成功或逾時（最多 1.5 秒）
  uint8_t buf[512];
  int n = 0;
  String host;
  unsigned long t = millis();
  while (millis() - t < 1500) {
    while (newClient.available() && n < (int)sizeof(buf)) {
      buf[n++] = newClient.read();
    }
    host = parseSNI(buf, n);
    if (host.length() > 0) break;   // 已解析到 SNI
    if (n >= 10 && host.length() == 0 && millis() - t > 500) break;  // 有資料但解析不出來
    delay(5);
  }
  if (n == 0) { newClient.stop(); return; }

  Serial.printf("[TLS] 443 連線 來自 %s  SNI: %s\n",
                newClient.remoteIP().toString().c_str(),
                host.length() ? host.c_str() : "(無)");

  if (host.length() == 0 || !isProxyHost(host)) {
    newClient.stop();  // 非白名單，直接拒絕
    return;
  }

  // 找空閒 session slot
  int slot = -1;
  for (int i = 0; i < 3; i++) {
    if (!proxySessions[i].active) { slot = i; break; }
  }
  if (slot < 0) {
    Serial.println(F("[TLS] 代理位置已滿，拒絕"));
    newClient.stop();
    return;
  }

  // ESP32 透過 STA 自行 DNS 解析取得真實 IP
  IPAddress realIP;
  if (!WiFi.hostByName(host.c_str(), realIP)) {
    Serial.printf("[TLS] DNS 查詢失敗: %s\n", host.c_str());
    newClient.stop();
    return;
  }

  // 透過 STA 連線至真實伺服器
  WiFiClient upstream;
  if (!upstream.connect(realIP, 443)) {
    Serial.printf("[TLS] 無法連線至 %s (%s)\n",
                  host.c_str(), realIP.toString().c_str());
    newClient.stop();
    return;
  }

  // 轉送已讀取的 ClientHello
  upstream.write(buf, n);

  // 建立代理 session
  proxySessions[slot].inbound  = newClient;
  proxySessions[slot].outbound = upstream;
  proxySessions[slot].active   = true;
  Serial.printf("[TLS] 代理建立 [%d]: %s → %s\n",
                slot, host.c_str(), realIP.toString().c_str());
}

// ================================================================
// UDP QUIC 攔截輔助
// ================================================================

void drainUdp(WiFiUDP &udp, uint16_t port) {
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    uint8_t buf[256];
    udp.read(buf, min(packetSize, (int)sizeof(buf)));
    Serial.printf("[UDP] QUIC 封包 UDP:%d 來自 %s:%d 大小: %d bytes（已丟棄）\n",
                  port, udp.remoteIP().toString().c_str(), udp.remotePort(),
                  packetSize);
  }
}

// ================================================================
// Setup
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n--- ESP32 數據轉發網關啟動 ---"));

  // 1. WiFi 雙模 (AP + STA)
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(ssid_ap, pass_ap);
  Serial.print(F("[WiFi] AP 模式啟動, SSID: "));
  Serial.println(ssid_ap);

  IPAddress apIP = WiFi.softAPIP();
  Serial.print(F("[WiFi] AP IP 位址: "));
  Serial.println(apIP);

  WiFi.begin(ssid_sta, pass_sta);
  Serial.print(F("[WiFi] 正在連接至外部 WiFi: "));
  Serial.println(ssid_sta);

  // 2. DNS 攔截（所有域名 → 192.168.4.1）
  dnsServer.start(53, "*", apIP);
  Serial.println(F("[DNS] DNS 攔截已啟動（全域 → 192.168.4.1）"));

  // 3. HTTP Server (port 80)
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/generate204", HTTP_GET, handle204);
  httpServer.on("/generate_204", HTTP_GET, handle204);
  httpServer.on("/gen_204", HTTP_GET, handle204);
  httpServer.on("/getDNList", HTTP_GET, handleGetDNList);
  httpServer.on("/getHttpDnsServerList", HTTP_GET, handleGetHttpDnsServerList);
  httpServer.on("/v2/d", HTTP_GET, handleHttpDnsQuery);
  httpServer.on("/connecttest.txt", HTTP_GET, handleConnectTest);
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();
  Serial.println(F("[HTTP] HTTP Server 已啟動於端口 80"));

  // 4. TCP 透明代理 (port 443)
  rawServer443.begin();
  Serial.println(F("[TLS] TCP 透明代理已啟動於端口 443（白名單轉發）"));

  // 4b. UDP QUIC 攔截（port 80 & 443）
  udpQuic80.begin(80);
  Serial.println(F("[UDP] QUIC 攔截已啟動於 UDP port 80"));
  udpQuic443.begin(443);
  Serial.println(F("[UDP] QUIC 攔截已啟動於 UDP port 443"));

  // 5. MQTT
  mqttClient.setServer(mqtt_server, mqtt_port);

  // 6. WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println(F("[WS] WebSocket Server 已啟動於端口 81"));
}

// ================================================================
// Loop
// ================================================================

void loop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();
  handleTcpProxy443();
  webSocket.loop();

  drainUdp(udpQuic80, 80);
  drainUdp(udpQuic443, 443);

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMqtt();
    }
    mqttClient.loop();
  }

  // STA 狀態監控
  static wl_status_t lastStatus = WL_IDLE_STATUS;
  wl_status_t currentStatus = WiFi.status();
  if (currentStatus != lastStatus) {
    if (currentStatus == WL_CONNECTED) {
      Serial.print(F("[WiFi] STA 已連接, IP: "));
      Serial.println(WiFi.localIP());
    } else if (currentStatus == WL_DISCONNECTED) {
      Serial.println(F("[WiFi] STA 連線中斷"));
    }
    lastStatus = currentStatus;
  }
}
