#pragma once
// Parse the standard-library free declarations before substituting calls in
// the logger. A command-line -Dfree also renames <cstdlib>'s declarations and
// breaks compilation on conforming toolchains.
#include "Arduino.h"
#include "ArduinoJson.h"
#include "esp_heap_caps.h"
#include "tusb.h"
#include <cstdlib>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
extern "C" void app_log_test_free(void*);
#define free app_log_test_free
