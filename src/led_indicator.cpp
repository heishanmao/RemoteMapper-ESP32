#include "led_indicator.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#ifndef RGB_BUILTIN
#define RGB_BUILTIN 48 // Default for most ESP32-S3 boards if not defined
#endif

static led_state_t s_current_base_state = LED_STATE_WAIT_CONNECTION;
static uint32_t s_flash_expire_time = 0;
static led_state_t s_flash_state = LED_STATE_WAIT_CONNECTION;
static bool s_is_flashing = false;

static uint32_t s_layer_color = 0x00FF00; // Default green for Layer 0
static bool s_layer_flash = false;
static bool s_low_battery = false;
static bool s_wifi_sleep = false;

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

static void update_hardware_led(led_state_t state) {
    if (s_layer_flash && s_is_flashing) {
        apply_led_color(dim_rgb(s_layer_color, LED_PULSE_BRIGHT));
        return;
    }

    // Low battery white double-pulse alert when connected
    if (s_low_battery && state == LED_STATE_CONNECTED) {
        uint32_t phase = millis() % 2000;
        if ((phase < 120) || (phase >= 220 && phase < 340)) {
            apply_led_color(0x0C0C0Cu); // dim white pulse
            return;
        }
    }

    // Wi-Fi sleeping or disabled: steady dim red = device asleep / web unreachable.
    if (s_wifi_sleep) {
        apply_led_color(dim_rgb(0xFF0000, LED_STEADY_BRIGHT));
        return;
    }

    switch (state) {
        case LED_STATE_WAIT_CONNECTION: {
            // Slow heartbeat instead of a steady red LED (150ms pulse / 4s).
            uint32_t phase = millis() % 4000;
            apply_led_color((phase < 150) ? 0x0C0000u : 0x020000u);
            break;
        }
        case LED_STATE_CONNECTED:
            apply_led_color(dim_rgb(s_layer_color, LED_STEADY_BRIGHT));
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
        bool fast = false;
        if (s_is_flashing) {
            fast = true;
            if (millis() > s_flash_expire_time) {
                s_is_flashing = false;
                update_hardware_led(s_current_base_state);
            } else {
                update_hardware_led(s_flash_state);
            }
        } else {
            update_hardware_led(s_current_base_state);
        }

        // Only animate when needed: flashes and the low-battery pulse run at
        // 20ms; the wait-connection heartbeat at 50ms; everything else 100ms.
        uint32_t tick_ms = 100;
        if (fast || (s_low_battery && s_current_base_state == LED_STATE_CONNECTED)) {
            tick_ms = 20;
        } else if (s_current_base_state == LED_STATE_WAIT_CONNECTION) {
            tick_ms = 50;
        }
        vTaskDelay(pdMS_TO_TICKS(tick_ms));
    }
}

void led_indicator_init(void) {
    update_hardware_led(LED_STATE_WAIT_CONNECTION);
    xTaskCreatePinnedToCore(led_task, "led_task", 2048, NULL, 1, NULL, 1);
}

void led_indicator_set(led_state_t state) {
    if (state == LED_STATE_HID_KEY_PRESS || state == LED_STATE_MIC_KEY_PRESS) return; // Use trigger for flashes
    s_is_flashing = false;
    s_current_base_state = state;
    update_hardware_led(state);
}

void led_indicator_trigger_key(bool is_voice_key) {
    s_layer_flash = false;
    s_flash_state = is_voice_key ? LED_STATE_MIC_KEY_PRESS : LED_STATE_HID_KEY_PRESS;
    s_flash_expire_time = millis() + 100; // Flash for 100ms
    s_is_flashing = true;
}

void led_indicator_set_wifi_sleep(bool is_sleep) {
    if (s_wifi_sleep == is_sleep) {
        return;
    }
    s_wifi_sleep = is_sleep;
    update_hardware_led(s_current_base_state);
}

void led_indicator_set_layer_color(uint32_t rgb_color) {
    s_layer_color = (rgb_color == 0) ? 0x00FF00 : rgb_color;
    s_layer_flash = true;
    s_flash_expire_time = millis() + 200; // 200ms flash on layer change
    s_is_flashing = true;
}

void led_indicator_set_low_battery(bool is_low) {
    s_low_battery = is_low;
}

