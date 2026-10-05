#pragma once
#include "Arduino.h"
#include <map>
#include <string>

namespace wifi_test_prefs {
struct State {
    std::map<std::string, std::string> values;
    int put_string_call = 0;
    int put_uint_call = 0;
    int put_bool_call = 0;
    int uint_reads = 0;
    int bool_reads = 0;
    int fail_string_call = -1;
    int fail_uint_call = -1;
    int fail_bool_call = -1;
    int corrupt_read_string_call = -1;
    int fail_uint_read_call = -1;
    int fail_bool_read_call = -1;
    int raw_string_reads = 0;
    int raw_uint_reads = 0;
    int raw_u8_reads = 0;
    int raw_string_sets = 0;
    int raw_commits = 0;
    int fail_raw_string_set_call = -1;
    int fail_raw_commit_call = -1;
    int fail_raw_string_read_call = -1;
    int corrupt_raw_string_read_call = -1;
    int fail_raw_uint_read_call = -1;
    int fail_raw_u8_read_call = -1;
    int string_reads = 0;
    bool begin_ok = true;
    bool omit_empty_key = false;
    int remove_calls = 0;
    int fail_remove_call = -1;
};
extern State state;
void reset();
}

class Preferences {
public:
    bool begin(const char*, bool) { return wifi_test_prefs::state.begin_ok; }
    bool isKey(const char* key) const {
        return wifi_test_prefs::state.values.count(key) != 0;
    }
    size_t putString(const char* key, const String& value) {
        auto& state = wifi_test_prefs::state;
        ++state.put_string_call;
        if (state.put_string_call == state.fail_string_call) return 0;
        const std::string text = (std::string)value;
        if (text.empty() && state.omit_empty_key) state.values.erase(key);
        else state.values[key] = text;
        return text.size();
    }
    String getString(const char* key, const String& fallback = String()) {
        auto& state = wifi_test_prefs::state;
        ++state.string_reads;
        if (state.string_reads == state.corrupt_read_string_call) return String("<corrupt>");
        const auto found = state.values.find(key);
        return found == state.values.end() ? fallback : String(found->second);
    }
    size_t putUInt(const char* key, uint32_t value) {
        auto& state = wifi_test_prefs::state;
        ++state.put_uint_call;
        if (state.put_uint_call == state.fail_uint_call) return 0;
        state.values[key] = std::to_string(value);
        return sizeof(value);
    }
    uint32_t getUInt(const char* key, uint32_t fallback = 0) const {
        auto& state = wifi_test_prefs::state;
        ++state.uint_reads;
        if (state.uint_reads == state.fail_uint_read_call) return fallback;
        const auto found = state.values.find(key);
        return found == wifi_test_prefs::state.values.end()
                ? fallback : (uint32_t)std::stoul(found->second);
    }
    size_t putBool(const char* key, bool value) {
        auto& state = wifi_test_prefs::state;
        ++state.put_bool_call;
        if (state.put_bool_call == state.fail_bool_call) return 0;
        state.values[key] = value ? "1" : "0";
        return sizeof(uint8_t);
    }
    bool getBool(const char* key, bool fallback = false) const {
        auto& state = wifi_test_prefs::state;
        ++state.bool_reads;
        if (state.bool_reads == state.fail_bool_read_call) return fallback;
        const auto found = state.values.find(key);
        return found == wifi_test_prefs::state.values.end()
                ? fallback : found->second == "1";
    }
    bool remove(const char* key) {
        auto& state = wifi_test_prefs::state;
        ++state.remove_calls;
        if (state.remove_calls == state.fail_remove_call) return false;
        return state.values.erase(key) != 0;
    }
};
