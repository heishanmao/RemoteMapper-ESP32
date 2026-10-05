#pragma once

#include <stdint.h>
#include <stddef.h>
#include <map>
#include <string>

class Preferences {
public:
    static inline std::map<std::string, uint8_t> values;
    static inline bool fail_begin = false;
    static inline bool fail_put = false;
    static inline bool fail_readback = false;
    static inline bool fail_remove = false;
    static inline uint32_t begin_calls = 0;

    bool begin(const char*, bool) {
        begin_calls++;
        return !fail_begin;
    }
    void end() {}
    size_t putUChar(const char* key, uint8_t value) {
        if (fail_put) return 0;
        values[key] = value;
        return 1;
    }
    uint8_t getUChar(const char* key, uint8_t fallback = 0) {
        if (fail_readback) return fallback;
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    bool isKey(const char* key) { return values.find(key) != values.end(); }
    bool remove(const char* key) {
        if (fail_remove) return false;
        return values.erase(key) != 0;
    }
};
