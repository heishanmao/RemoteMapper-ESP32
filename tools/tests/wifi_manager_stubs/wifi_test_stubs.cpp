#include "WiFi.h"
#include "ESPmDNS.h"
#include "Preferences.h"
#include "nvs.h"
#include <cstring>
#include <string>
WiFiClass WiFi;
MDNSClass MDNS;
namespace wifi_test_log { std::string last_line; }
namespace wifi_test_prefs {
State state;
void reset() { state = State{}; }
}

esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* output, size_t* length) {
    if (!length) return 3;
    auto& state = wifi_test_prefs::state;
    const auto found = state.values.find(key);
    if (found == state.values.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t required = found->second.size() + 1;
    if (!output) { *length = required; return ESP_OK; }
    ++state.raw_string_reads;
    if (state.raw_string_reads == state.fail_raw_string_read_call) return 4;
    if (*length < required) return 5;
    if (state.raw_string_reads == state.corrupt_raw_string_read_call) {
        std::memset(output, '#', required - 1);
        output[required - 1] = '\0';
    } else {
        std::memcpy(output, found->second.c_str(), required);
    }
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t, const char* key, uint32_t* output) {
    if (!output) return 3;
    auto& state = wifi_test_prefs::state;
    ++state.raw_uint_reads;
    if (state.raw_uint_reads == state.fail_raw_uint_read_call) return 4;
    const auto found = state.values.find(key);
    if (found == state.values.end()) return ESP_ERR_NVS_NOT_FOUND;
    *output = (uint32_t)std::stoul(found->second);
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* output) {
    if (!output) return 3;
    auto& state = wifi_test_prefs::state;
    ++state.raw_u8_reads;
    if (state.raw_u8_reads == state.fail_raw_u8_read_call) return 4;
    const auto found = state.values.find(key);
    if (found == state.values.end()) return ESP_ERR_NVS_NOT_FOUND;
    *output = (uint8_t)std::stoul(found->second);
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value) {
    auto& state = wifi_test_prefs::state;
    ++state.raw_string_sets;
    if (state.raw_string_sets == state.fail_raw_string_set_call) return 4;
    if ((!value || !*value) && state.omit_empty_key) state.values.erase(key);
    else state.values[key] = value ? value : "";
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t) {
    auto& state = wifi_test_prefs::state;
    ++state.raw_commits;
    return state.raw_commits == state.fail_raw_commit_call ? 4 : ESP_OK;
}
