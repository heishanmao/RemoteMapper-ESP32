#pragma once
#include <stddef.h>
#include <stdint.h>
bool tud_cdc_n_connected(uint8_t instance);
uint32_t tud_cdc_n_write(uint8_t instance, const void* buffer, uint32_t size);
uint32_t tud_cdc_n_write_flush(uint8_t instance);
