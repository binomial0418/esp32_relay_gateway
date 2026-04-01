# ESP32 Data Relay Gateway

![ESP32](https://img.shields.io/badge/Platform-ESP32-blue?logo=espressif)
![Arduino](https://img.shields.io/badge/Framework-Arduino-00979C?logo=arduino)

這是一個基於 ESP32 的數據轉發網關。它能同時作為 **Access Point (AP)** 接收手持裝置的數據，並透過 **Station (STA)** 模式連線至外部 WiFi 將數據轉發至 **MQTT Broker**。

## 🌟 核心功能

-   **雙模運作 (AP + STA)**：
    -   **AP 模式**：提供內網 SSID (`nx4_obd_relay`)，供手持裝置連線並透過 WebSocket 發送數據。
    -   **STA 模式**：連線至外部 WiFi 以訪問 MQTT Broker。
-   **Captive Portal (強制跳轉門戶)**：
    -   內建 DNS 攔截技術，將所有域名導向 AP IP (`192.168.4.1`)。
    -   針對 iOS/Android 提供 HTTP 204 回應，實現無縫的手機彈窗或連線偵測。
-   **WebSocket to MQTT 橋接**：
    -   在 **Port 81** 啟動 WebSocket Server。
    -   收到 WebSocket TEXT 數據後即時轉發至指定的 MQTT Topic。
-   **穩定性優化**：
    -   使用 **Non-blocking (非同步)** 重連機制，確保在網路不穩時不會導致主循環 (Loop) 阻塞。
    -   大量使用 `F()` 宏優化記憶體，適合長效運行。

## 🛠️ 硬體與環境需求

-   **硬體**：任何 ESP32 開發板 (如 DevKit V1)。
-   **開發環境**：
    -   Arduino IDE 或 `arduino-cli` (推薦)。
    -   ESP32 Core (建議版本 2.0.x 或 3.0.x)。
-   **相依函式庫**：
    -   `PubSubClient` (by Nick O'Leary)
    -   `WebSockets` (by Markus Sattler)

## ⚙️ 設定指南

本專案使用 `config.h` 進行參數管理。

1.  參考 `config.h.example` 建立 `config.h` 檔案。
2.  編輯 `config.h` 中的以下參數：
    -   `ssid_sta` / `pass_sta`：外部 WiFi 連線資訊。
    -   `ssid_ap` / `pass_ap`：ESP32 AP 模式設定。
    -   `mqtt_server` / `mqtt_port` / `mqtt_topic`：MQTT Broker 詳細資訊。

## 🚀 編譯與燒錄

### 使用 arduino-cli (推薦)

```bash
# 編譯專案
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_relay_gateway.ino

# 上傳至 ESP32 (速率預設 460800)
arduino-cli upload -p /dev/tty.SLAB_USBtoUART --fqbn esp32:esp32:esp32 --port-config 460800 esp32_relay_gateway.ino
```

## 📝 專案結構

-   `esp32_relay_gateway.ino`：主程式邏輯與連線管理。
-   `config.h`：各類參數配置（未納入 Git 以保護金鑰）。
-   `config.h.example`：配置範本。

## 授權
MIT License
