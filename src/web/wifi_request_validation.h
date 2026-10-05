#pragma once

#include "wifi/wifi_manager.h"
#include <ArduinoJson.h>

struct wifi_power_request_t {
    bool set_policy;
    wifi_policy_t policy;
    bool set_timeout_min;
    uint32_t timeout_min;
    bool set_timeout_enabled;
    bool timeout_enabled;
};

inline bool wifi_parse_sta_request(JsonDocument& doc, String* ssid, String* password) {
    if (!ssid || !password || !doc.is<JsonObject>() || !doc["ssid"].is<const char*>() ||
            (!doc["pass"].isUnbound() && !doc["pass"].is<const char*>())) {
        return false;
    }
    *ssid = doc["ssid"].as<String>();
    *password = doc["pass"] | "";
    return ssid->length() > 0 && ssid->length() <= 32 && password->length() <= 64;
}

inline bool wifi_parse_ap_request(JsonDocument& doc, String* password) {
    if (!password || !doc.is<JsonObject>() || !doc["ap_pass"].is<const char*>()) return false;
    *password = doc["ap_pass"].as<String>();
    password->trim();
    return password->length() == 0 ||
           (password->length() >= 8 && password->length() <= 63);
}

inline bool wifi_parse_power_request(JsonDocument& doc, wifi_power_request_t* request) {
    if (!request || !doc.is<JsonObject>()) return false;
    request->set_policy = !doc["policy"].isUnbound();
    request->set_timeout_min = !doc["timeout_min"].isUnbound();
    request->set_timeout_enabled = !doc["timeout_enabled"].isUnbound();
    request->policy = WIFI_POLICY_ON_DEMAND;
    request->timeout_min = 0;
    request->timeout_enabled = false;

    if (request->set_policy) {
        if (!doc["policy"].is<int>()) return false;
        const int policy = doc["policy"].as<int>();
        if (policy < (int)WIFI_POLICY_ALWAYS_ON || policy > (int)WIFI_POLICY_DISABLED) return false;
        request->policy = (wifi_policy_t)policy;
    }
    if (request->set_timeout_min) {
        if (!doc["timeout_min"].is<int>()) return false;
        const int timeout = doc["timeout_min"].as<int>();
        if (timeout != 0 && timeout != 1 && timeout != 2 && timeout != 5 &&
                timeout != 10 && timeout != 30) return false;
        request->timeout_min = (uint32_t)timeout;
    }
    if (request->set_timeout_enabled) {
        if (!doc["timeout_enabled"].is<bool>()) return false;
        request->timeout_enabled = doc["timeout_enabled"].as<bool>();
    }
    return true;
}
