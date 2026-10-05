#pragma once
#include "WiFi.h"
enum class DNSReplyCode { NoError };
class DNSServer {
public:
    void stop() {}
    void setErrorReplyCode(DNSReplyCode) {}
    bool start(uint16_t, const char*, const IPAddress&) { return true; }
    void processNextRequest() {}
};
