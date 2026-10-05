#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

class String {
public:
    void reserve(size_t n) { value.reserve(n); }
    String& operator+=(const char* text) { value += text; return *this; }
    const std::string& str() const { return value; }
private:
    std::string value;
};

class TestSerial {
public:
    int available_limit = 4096;
    size_t write_limit = (size_t)-1;
    explicit operator bool() const { return true; }
    int availableForWrite() const { return available_limit; }
    size_t write(const uint8_t* data, size_t size);
    size_t write(uint8_t value) { return write(&value, 1); }
};

extern TestSerial Serial;
uint32_t millis(void);

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
