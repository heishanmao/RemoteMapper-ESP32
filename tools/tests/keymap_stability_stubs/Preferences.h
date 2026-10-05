#pragma once
#include "Arduino.h"
#include <map>
#include <string>

class Preferences {
public:
    static inline std::map<std::string, std::string> values;
    static inline bool fail_writes = false;
    bool begin(const char*, bool) { return true; }
    void end() {}
    size_t putString(const char* key, const String& value) {
        if (fail_writes) return 0;
        values[key] = value.c_str();
        return value.size();
    }
    String getString(const char* key, const char* fallback = "") {
        auto it = values.find(key);
        return it == values.end() ? String(fallback) : String(it->second);
    }
    bool remove(const char* key) { values.erase(key); return true; }
};
