#pragma once
#include "Arduino.h"

class IPAddress {
    uint8_t bytes_[4]{};
public:
    IPAddress() = default;
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes_{a,b,c,d} {}
    uint8_t operator[](size_t index) const { return bytes_[index]; }
    String toString() const {
        return String(std::to_string(bytes_[0]) + "." + std::to_string(bytes_[1]) + "." +
                std::to_string(bytes_[2]) + "." + std::to_string(bytes_[3]));
    }
};

enum wifi_mode_t { WIFI_OFF, WIFI_STA, WIFI_AP, WIFI_AP_STA };
enum wifi_auth_mode_t { WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK };
static constexpr int WL_DISCONNECTED = 0;
static constexpr int WL_CONNECTED = 3;
static constexpr int WIFI_SCAN_RUNNING = -1;
static constexpr int WIFI_SCAN_FAILED = -2;
static constexpr int WIFI_POWER_8_5dBm = 1;

class WiFiClass {
public:
    int begin_calls = 0;
    int softap_calls = 0;
    int status() const { return WL_CONNECTED; }
    void mode(wifi_mode_t) {}
    void softAPConfig(const IPAddress&, const IPAddress&, const IPAddress&) {}
    bool softAP(const char*, const char*) { ++softap_calls; return true; }
    bool softAPdisconnect(bool) { return true; }
    IPAddress softAPIP() const { return IPAddress(192,168,4,1); }
    IPAddress localIP() const { return IPAddress(192,168,1,20); }
    void begin(const char*, const char*) { ++begin_calls; }
    void disconnect(bool = true) {}
    void setSleep(bool) {}
    bool setTxPower(int) { return true; }
    int scanNetworks(bool = false) { return 0; }
    int scanComplete() const { return 0; }
    void scanDelete() {}
    String SSID(int) const { return String(); }
    int32_t RSSI(int) const { return -40; }
    wifi_auth_mode_t encryptionType(int) const { return WIFI_AUTH_OPEN; }
    int8_t RSSI() const { return -40; }
};
extern WiFiClass WiFi;
