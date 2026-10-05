#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class String {
    std::string value_;
public:
    String() = default;
    String(const char* value) : value_(value ? value : "") {}
    String(const std::string& value) : value_(value) {}
    size_t length() const { return value_.size(); }
    const char* c_str() const { return value_.c_str(); }
    char charAt(size_t index) const { return value_.at(index); }
    void reserve(size_t) {}
    void trim() {
        const size_t first = value_.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) { value_.clear(); return; }
        const size_t last = value_.find_last_not_of(" \t\r\n");
        value_ = value_.substr(first, last - first + 1);
    }
    String& operator+=(char value) { value_.push_back(value); return *this; }
    String& operator+=(const char* value) { value_ += value ? value : ""; return *this; }
    String& operator+=(const String& value) { value_ += value.value_; return *this; }
    bool operator==(const String& other) const { return value_ == other.value_; }
    bool operator!=(const String& other) const { return !(*this == other); }
    bool operator==(const char* other) const { return value_ == (other ? other : ""); }
    bool operator!=(const char* other) const { return !(*this == other); }
    bool concat(const char* value) { if (!value) return false; value_ += value; return true; }
    size_t write(uint8_t value) { value_.push_back((char)value); return 1; }
    size_t write(const uint8_t* bytes, size_t size) {
        value_.append(reinterpret_cast<const char*>(bytes), size); return size;
    }
    operator std::string() const { return value_; }
};

inline uint32_t millis() { static uint32_t now = 100; return ++now; }
inline void delay(uint32_t) {}
