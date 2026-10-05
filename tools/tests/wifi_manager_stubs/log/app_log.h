#pragma once
#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <string>
namespace wifi_test_log { extern std::string last_line; }
inline void app_log(const char*, const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    wifi_test_log::last_line = message;
}
