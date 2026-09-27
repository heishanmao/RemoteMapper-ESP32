#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool uac_microphone_init(void);
void uac_microphone_task(void);
bool uac_microphone_is_streaming(void);
void uac_microphone_get_control(uint8_t* mute, int16_t* volume);
uint8_t uac_microphone_get_alt(void);

#ifdef __cplusplus
}
#endif
