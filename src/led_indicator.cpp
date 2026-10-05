#include "led_indicator.h"
#include "app_config.h"
#include "core_diagnostics.h"
#include "log/app_log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#ifndef RGB_BUILTIN
#define RGB_BUILTIN 48 // Default for most ESP32-S3 boards if not defined
#endif

struct LedState {
    led_state_t base = LED_STATE_WAIT_CONNECTION;
    uint32_t flash_until_ms = 0;
    led_state_t flash = LED_STATE_WAIT_CONNECTION;
    bool flashing = false;
    uint32_t layer_color = 0x00FF00;
    bool layer_flash = false;
    bool low_battery = false;
    bool wifi_sleep = false;
};
static LedState s_state;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_led_task = nullptr;

// Setters run from BLE and management tasks. Only the LED task touches RMT;
// publishing a small state snapshot never waits for the peripheral.
static void wake_led_task() {
    if (s_led_task) xTaskNotifyGive(s_led_task);
}

// Master brightness levels (0..255). The DevKit's WS2812 is very visible even
// at low duty, so steady states stay extremely dim and short pulses carry the
// feedback. Tune these two numbers to taste.
#define LED_STEADY_BRIGHT   6   // steady/idle states (was ~20-24)
#define LED_PULSE_BRIGHT    12  // flashes / heartbeat / alert pulses (was ~24-36)

// Last colour actually pushed to the LED. neopixelWrite drives the RMT
// peripheral with tight timing, so only write when the colour really changes.
static uint32_t s_last_rgb = 0xFFFFFFFFu;

static uint32_t dim_rgb(uint32_t rgb, uint8_t level) {
    uint8_t r = (uint8_t)(((rgb >> 16) & 0xFF) * level / 255);
    uint8_t g = (uint8_t)(((rgb >> 8) & 0xFF) * level / 255);
    uint8_t b = (uint8_t)((rgb & 0xFF) * level / 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void apply_led_color(uint32_t rgb) {
    if (s_last_rgb == rgb) {
        return;
    }
    s_last_rgb = rgb;
    neopixelWrite(RGB_BUILTIN, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static void update_hardware_led(const LedState& snapshot, uint32_t now_ms) {
    const led_state_t state = snapshot.flashing ? snapshot.flash : snapshot.base;
    if (snapshot.layer_flash && snapshot.flashing) {
        apply_led_color(dim_rgb(snapshot.layer_color, LED_PULSE_BRIGHT));
        return;
    }

    // Low battery white double-pulse alert when connected
    if (snapshot.low_battery && state == LED_STATE_CONNECTED) {
        uint32_t phase = now_ms % 2000;
        if ((phase < 120) || (phase >= 220 && phase < 340)) {
            apply_led_color(0x0C0C0Cu); // dim white pulse
            return;
        }
    }

    // Wi-Fi sleeping or disabled: steady dim red = device asleep / web unreachable.
    if (snapshot.wifi_sleep) {
        apply_led_color(dim_rgb(0xFF0000, LED_STEADY_BRIGHT));
        return;
    }

    switch (state) {
        case LED_STATE_WAIT_CONNECTION: {
            // Slow heartbeat instead of a steady red LED (150ms pulse / 4s).
            uint32_t phase = now_ms % 4000;
            apply_led_color((phase < 150) ? 0x0C0000u : 0x020000u);
            break;
        }
        case LED_STATE_CONNECTED:
            apply_led_color(dim_rgb(snapshot.layer_color, LED_STEADY_BRIGHT));
            break;
        case LED_STATE_MIC_STREAMING:
            apply_led_color(0x00000Cu); // dim blue
            break;
        case LED_STATE_HID_KEY_PRESS:
            apply_led_color(0x0C0C00u); // dim yellow flash
            break;
        case LED_STATE_MIC_KEY_PRESS:
            apply_led_color(0x0C0000u); // dim red flash
            break;
        default:
            apply_led_color(0x000000u); // Off
            break;
    }
}

static void led_task(void *arg) {
    while (1) {
        const uint32_t now_ms = millis();
        taskENTER_CRITICAL(&s_state_mux);
        if (s_state.flashing && (int32_t)(now_ms - s_state.flash_until_ms) >= 0)
            s_state.flashing = false;
        const LedState snapshot = s_state;
        taskEXIT_CRITICAL(&s_state_mux);
        update_hardware_led(snapshot, now_ms);

        // Only animate when needed: flashes and the low-battery pulse run at
        // 20ms; the wait-connection heartbeat at 50ms; everything else 100ms.
        uint32_t tick_ms = 100;
        if (snapshot.flashing || (snapshot.low_battery && snapshot.base == LED_STATE_CONNECTED)) {
            tick_ms = 20;
        } else if (snapshot.base == LED_STATE_WAIT_CONNECTION) {
            tick_ms = 50;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tick_ms));
    }
}

bool led_indicator_init(void) {
    if (s_led_task) return true;
    if (xTaskCreatePinnedToCore(led_task, "led_task", 2048, NULL, PRIO_TASK_LED, &s_led_task, TASK_CORE_LED) != pdPASS) {
        app_log("LED", "Failed to spawn indicator task");
        return false;
    }
    core_diagnostics_register("led_task", s_led_task, TASK_CORE_LED);
    return true;
}

void led_indicator_set(led_state_t state) {
    if (state == LED_STATE_HID_KEY_PRESS || state == LED_STATE_MIC_KEY_PRESS) return; // Use trigger for flashes
    taskENTER_CRITICAL(&s_state_mux);
    s_state.flashing = false;
    s_state.base = state;
    taskEXIT_CRITICAL(&s_state_mux);
    wake_led_task();
}

void led_indicator_trigger_key(bool is_voice_key) {
    const uint32_t until = millis() + 100;
    taskENTER_CRITICAL(&s_state_mux);
    s_state.layer_flash = false;
    s_state.flash = is_voice_key ? LED_STATE_MIC_KEY_PRESS : LED_STATE_HID_KEY_PRESS;
    s_state.flash_until_ms = until;
    s_state.flashing = true;
    taskEXIT_CRITICAL(&s_state_mux);
    wake_led_task();
}

void led_indicator_trigger_stuck(void) {
    const uint32_t until = millis() + 300;
    taskENTER_CRITICAL(&s_state_mux);
    s_state.layer_flash = false;
    s_state.flash = LED_STATE_MIC_KEY_PRESS;
    s_state.flash_until_ms = until;
    s_state.flashing = true;
    taskEXIT_CRITICAL(&s_state_mux);
    wake_led_task();
}

void led_indicator_set_wifi_sleep(bool is_sleep) {
    taskENTER_CRITICAL(&s_state_mux);
    const bool changed = s_state.wifi_sleep != is_sleep;
    s_state.wifi_sleep = is_sleep;
    taskEXIT_CRITICAL(&s_state_mux);
    if (changed) wake_led_task();
}

void led_indicator_set_layer_color(uint32_t rgb_color) {
    const uint32_t until = millis() + 200;
    taskENTER_CRITICAL(&s_state_mux);
    s_state.layer_color = (rgb_color == 0) ? 0x00FF00 : rgb_color;
    s_state.layer_flash = true;
    s_state.flash_until_ms = until;
    s_state.flashing = true;
    taskEXIT_CRITICAL(&s_state_mux);
    wake_led_task();
}

void led_indicator_set_low_battery(bool is_low) {
    taskENTER_CRITICAL(&s_state_mux);
    s_state.low_battery = is_low;
    taskEXIT_CRITICAL(&s_state_mux);
    wake_led_task();
}

