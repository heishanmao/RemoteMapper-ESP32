#include "log/app_log.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "esp_heap_caps.h"
#include "tusb.h"

TestSerial Serial;
static std::vector<void*> tracked_allocations;
static std::string serial_output;
static std::string cdc_output;
static bool cdc_connected = false;
static uint32_t clock_ms = 1234;

extern "C" void app_log_test_free(void* p) {
    auto it = std::find(tracked_allocations.begin(), tracked_allocations.end(), p);
    if (it != tracked_allocations.end()) tracked_allocations.erase(it);
    std::free(p);
}

void* heap_caps_malloc(size_t size, unsigned int) {
    void* p = std::malloc(size);
    if (p) tracked_allocations.push_back(p);
    return p;
}

uint32_t millis(void) { return clock_ms++; }

size_t TestSerial::write(const uint8_t* data, size_t size) {
    serial_output.append(reinterpret_cast<const char*>(data), size);
    return size;
}

bool tud_cdc_n_connected(uint8_t instance) { return instance == 0 && cdc_connected; }

uint32_t tud_cdc_n_write(uint8_t instance, const void* buffer, uint32_t size) {
    if (instance != 0 || !cdc_connected) return 0;
    cdc_output.append(static_cast<const char*>(buffer), size);
    return size;
}

uint32_t tud_cdc_n_write_flush(uint8_t) { return 0; }

static void drain(size_t calls) {
    for (size_t i = 0; i < calls; ++i) app_log_task();
}

int main() {
    app_log_init();
    assert(tracked_allocations.size() == 1); // the log ring

    app_log_set_console_enabled(false);
    app_log_set_cdc_enabled(false);
    app_log("TEST", "before-clear");
    app_log_clear();
    app_log("TEST", "after-clear");
    std::string json = app_log_get_json().str();
    assert(json.find("after-clear") != std::string::npos);
    assert(json.find("before-clear") == std::string::npos);

    app_log_set_console_enabled(true);
    app_log("TEST", "deferred-output");
    assert(serial_output.empty()); // callback-side call never writes Serial
    drain(4);
    assert(serial_output.find("deferred-output") != std::string::npos);
    assert(serial_output.back() == '\n');

    // A full snprintf line must leave room for the mirror's newline and NUL.
    serial_output.clear();
    const std::string long_tag(100, 'T');
    const std::string long_message(300, 'M');
    app_log(long_tag.c_str(), "%s", long_message.c_str());
    drain(6);
    assert(serial_output.size() <= 159);
    assert(serial_output.back() == '\n');

    app_log_set_console_enabled(false);
    for (int i = 0; i < 260; ++i) app_log("RING", "line-%03d", i);
    json = app_log_get_json().str();
    assert(json.find("line-259") != std::string::npos);
    assert(json.find("line-140") != std::string::npos);
    assert(json.find("line-139") == std::string::npos);

    std::string large(16384, 'J');
    cdc_connected = true;
    assert(app_log_queue_cdc_text(large.data(), large.size()));
    assert(tracked_allocations.size() == 2);
    drain((large.size() + 31) / 32 + 1);
    assert(cdc_output == large);
    assert(tracked_allocations.size() == 1); // drained payload was freed

    serial_output.clear();
    assert(app_log_queue_cli_text(APP_LOG_OUTPUT_UART, large.data(), large.size()));
    assert(tracked_allocations.size() == 2);
    drain((large.size() + 31) / 32 + 1);
    assert(serial_output == large); // UART uses the owned 16 KB payload too
    assert(tracked_allocations.size() == 1);

    for (int i = 0; i < 16; ++i) {
        const std::string item = "payload-" + std::to_string(i);
        assert(app_log_queue_cli_text(APP_LOG_OUTPUT_UART, item.data(), item.size()));
    }
    const uint32_t dropped_before = app_log_get_cli_dropped(APP_LOG_OUTPUT_UART);
    assert(!app_log_queue_cli_text(APP_LOG_OUTPUT_UART, "whole-message-rejected", 22));
    assert(app_log_get_cli_dropped(APP_LOG_OUTPUT_UART) == dropped_before + 1);
    assert(tracked_allocations.size() == 17); // failed enqueue freed its copy

    drain(18); // all queued lines, then the reserved fixed-size error notice
    assert(serial_output.find("output_queue_full") != std::string::npos);
    assert(tracked_allocations.size() == 1);
    const std::string oversize(16385, 'X');
    cdc_output.clear();
    assert(!app_log_queue_cli_text(APP_LOG_OUTPUT_CDC, oversize.data(), oversize.size()));
    assert(tracked_allocations.size() == 1);

    cdc_connected = true;
    drain(2); // clear the oversize error notice before exercising queue-full
    assert(cdc_output == "\r\n{\"error\":\"output_queue_full\"}\r\n");
    for (int i = 0; i < 16; ++i) {
        assert(app_log_queue_cli_text(APP_LOG_OUTPUT_CDC, "cdc", 3));
    }
    assert(!app_log_queue_cli_text(APP_LOG_OUTPUT_CDC, "rejected", 8));
    cdc_connected = false;
    drain(18); // disconnected CDC releases payloads and its pending error
    assert(tracked_allocations.size() == 1);
    return 0;
}
