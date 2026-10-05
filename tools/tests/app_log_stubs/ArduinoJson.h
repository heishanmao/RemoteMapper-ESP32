#pragma once
#include "Arduino.h"
#include <string>
#include <vector>

class JsonDocument;
class JsonArray {
public:
    explicit JsonArray(JsonDocument* owner = nullptr) : doc(owner) {}
    void add(const char* value);
private:
    JsonDocument* doc;
};

class JsonDocument {
public:
    class Key {
    public:
        explicit Key(JsonDocument* owner) : doc(owner) {}
        template <typename T> T to() { return T(doc); }
    private:
        JsonDocument* doc;
    };
    Key operator[](const char*) { return Key(this); }
    std::vector<std::string> logs;
};

inline void JsonArray::add(const char* value) { doc->logs.emplace_back(value); }

inline size_t serializeJson(const JsonDocument& doc, String& out) {
    out += "{\"logs\":[";
    for (size_t i = 0; i < doc.logs.size(); ++i) {
        if (i) out += ",";
        out += "\"";
        for (char c : doc.logs[i]) {
            if (c == '"' || c == '\\') {
                char escaped[3] = {'\\', c, '\0'};
                out += escaped;
            } else {
                char one[2] = {c, '\0'};
                out += one;
            }
        }
        out += "\"";
    }
    out += "]}";
    return 0;
}
