#pragma once

#define FIRMWARE_NAME       "RemoteMapper-ESP32"
#if defined(REMOTEMAPPER_DWC2_DRIVER)
#define FIRMWARE_VERSION    "1.2.53"
#else
#define FIRMWARE_VERSION    "1.2.53-legacy-usb"
#endif
#define HARDWARE_TARGET     "ESP32-S3-N16R8"
#define AUTHOR              "RemoteMapper Project"
