#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. Xiaomi Remote Physical Key Codes (HOGP / HID raw bytes)
// ==========================================
#define MI_KEY_VOL_UP       0x80    // Volume Up
#define MI_KEY_VOL_DOWN     0x81    // Volume Down
#define MI_KEY_BACK         0xF1    // Back key
#define MI_KEY_POWER        0x66    // Power button
#define MI_KEY_POWER_ALT    0xFF
#define MI_KEY_HOME         0x24    // Home button
#define MI_KEY_HOME_ALT     0x4A
#define MI_KEY_MENU         0x5D    // Menu button
#define MI_KEY_MENU_ALT     0x65
#define MI_KEY_TV           0xC0    // TV button
#define MI_KEY_TV_ALT       0x35
#define MI_KEY_UP           0x52    // D-Pad Up
#define MI_KEY_DOWN         0x51    // D-Pad Down
#define MI_KEY_LEFT         0x50    // D-Pad Left
#define MI_KEY_RIGHT        0x4F    // D-Pad Right
#define MI_KEY_OK           0x28    // OK / Enter
#define MI_KEY_VOICE        0x04    // Voice button (ATVV)
#define MI_KEY_VOICE_ALT    0x3E    // Voice button (HOGP)

// Chromecast-only physical keys (indexed consumer report). Never emitted by Xiaomi.
#define MI_KEY_MUTE         0x90    // Mute
#define MI_KEY_YOUTUBE      0x91    // YouTube shortcut
#define MI_KEY_NETFLIX      0x92    // Netflix shortcut

// ==========================================
// 2. USB HID Keyboard Modifier Bitmasks
// ==========================================
#define USB_MOD_NONE        0x00
#define USB_MOD_LCTRL       0x01
#define USB_MOD_LSHIFT      0x02
#define USB_MOD_LALT        0x04
#define USB_MOD_LGUI        0x08    // Left Win Key
#define USB_MOD_RCTRL       0x10
#define USB_MOD_RSHIFT      0x20
#define USB_MOD_RALT        0x40    // Right Alt Key (AltGr)
#define USB_MOD_RGUI        0x80    // Right Win Key

// ==========================================
// 3. Standard USB HID Keyboard Key Codes
// ==========================================
#define USB_KEY_NONE        0x00
#define USB_KEY_A           0x04
#define USB_KEY_D           0x07    // 'D' (for Win+D Show Desktop)
#define USB_KEY_H           0x0B    // 'H' (for Win+H Voice Typing)
#define USB_KEY_RETURN      0x28    // Enter
#define USB_KEY_ESCAPE      0x29    // Esc
#define USB_KEY_BACKSPACE   0x2A
#define USB_KEY_TAB         0x2B    // Tab (for Alt+Tab Task Switch)
#define USB_KEY_SPACE       0x2C    // Spacebar
#define USB_KEY_F5          0x3E    // F5 Refresh
#define USB_KEY_F8          0x41    // F8 Voice IME hotkey
#define USB_KEY_RIGHT       0x4F    // Arrow Right
#define USB_KEY_LEFT        0x50    // Arrow Left
#define USB_KEY_DOWN        0x51    // Arrow Down
#define USB_KEY_UP          0x52    // Arrow Up
#define USB_KEY_COMMA       0x36    // ',' (for RAlt+, WeChat IME hotkey)

// ==========================================
// 4. USB HID Consumer Control Usages
// ==========================================
#define USB_CONSUMER_NONE           0x0000
#define USB_CONSUMER_POWER          0x0030
#define USB_CONSUMER_SLEEP          0x0032
#define USB_CONSUMER_PLAY_PAUSE     0x00CD
#define USB_CONSUMER_MUTE           0x00E2
#define USB_CONSUMER_VOLUME_UP      0x00E9
#define USB_CONSUMER_VOLUME_DOWN    0x00EA
#define USB_CONSUMER_NEXT_TRACK     0x00B5
#define USB_CONSUMER_PREV_TRACK     0x00B6
#define USB_CONSUMER_AC_HOME        0x0223
#define USB_CONSUMER_AC_BACK        0x0224

#ifdef __cplusplus
}
#endif
