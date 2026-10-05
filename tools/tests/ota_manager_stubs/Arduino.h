#pragma once

#include <stdint.h>
#include <string>

class String {
public:
    String() = default;
    explicit String(uint32_t value) : value_(std::to_string(value)) {}
    const char* c_str() const { return value_.c_str(); }
private:
    std::string value_;
};

uint32_t millis(void);
void delay(uint32_t ms);

struct TestEsp {
    void restart() {}
};
extern TestEsp ESP;
