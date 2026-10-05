#pragma once
#include <string>
#include <cstdint>

class String {
    std::string value_;
public:
    String() = default;
    String(const char* s) : value_(s ? s : "") {}
    String(const std::string& s) : value_(s) {}
    String& operator=(const char* s) { value_ = s ? s : ""; return *this; }
    const char* c_str() const { return value_.c_str(); }
    const char* data() const { return value_.data(); }
    size_t size() const { return value_.size(); }
    size_t length() const { return value_.size(); }
    bool startsWith(const char* p) const { return value_.rfind(p, 0) == 0; }
    bool concat(const char* s) { if (!s) return false; value_ += s; return true; }
    size_t write(uint8_t c) { value_.push_back(static_cast<char>(c)); return 1; }
    size_t write(const uint8_t* bytes, size_t n) { value_.append(reinterpret_cast<const char*>(bytes), n); return n; }
    String& operator+=(const char* s) { value_ += s ? s : ""; return *this; }
    String& operator+=(const String& s) { value_ += s.value_; return *this; }
    String& operator+=(const std::string& s) { value_ += s; return *this; }
    bool operator==(const String& other) const { return value_ == other.value_; }
    bool operator==(const char* other) const { return value_ == (other ? other : ""); }
    bool operator!=(const String& other) const { return !(*this == other); }
    bool operator!=(const char* other) const { return !(*this == other); }
    bool isEmpty() const { return value_.empty(); }
    void trim() {
        size_t a = value_.find_first_not_of(" \t\r\n");
        size_t b = value_.find_last_not_of(" \t\r\n");
        value_ = a == std::string::npos ? "" : value_.substr(a, b - a + 1);
    }
};
