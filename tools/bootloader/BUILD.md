# RemoteMapper Custom OTA Bootloader (ESP32-S3, IDF v4.4.7)

## Why this exists

The Arduino-ESP32 framework ships a **single-app** bootloader (its sdkconfig has
`PARTITION_TABLE_SINGLE_APP`), which **ignores the otadata partition** and always
boots the first app slot — so Web OTA uploads can never switch slots. This project
builds a proper **dual-OTA** bootloader from ESP-IDF v4.4.7 source with INFO logs
on UART0.

- `bootloader_esp32s3_ota_16mb.bin` — prebuilt binary (21168 bytes, dio/80m/16MB), flash at `0x0`
- `idfbl/` — minimal IDF project sources used to build it

## Flashing

```
python esptool.py --chip esp32s3 --port COMx --baud 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 16MB 0x0 bootloader_esp32s3_ota_16mb.bin
```

The rest of the partition layout is untouched (see `idfbl/partitions.csv`, which
matches the Arduino `default_16MB.csv`).

## How the bootloader selects the boot slot

- otadata lives at `0xE000` (copy 0) and `0xF000` (copy 1), 32-byte entries:
  `ota_seq:u32 | seq_label:20B | ota_state:u32 | crc:u32`
- **CRC is computed over ONLY the 4 bytes of `ota_seq`** with
  `esp_rom_crc32_le(UINT32_MAX, &seq, 4)` — reflected CRC-32, **init=0, final XOR
  0xFFFFFFFF**. Note: Python's `zlib.crc32()` does NOT equal this
  (`zlib.crc32(data)` = init 0xFFFFFFFF + final XOR; `~zlib.crc32(data) & 0xFFFFFFFF`
  ... use the snippet below).
- Slot selection: `boot_index = (ota_seq - 1) % app_count` → seq odd ⇒ app0,
  seq even ⇒ app1. The bootloader picks the **newer valid copy** (higher seq).
- Known-good CRC vectors: seq=1 → `0x4743989A`, seq=18 → `0x05EF60EB`,
  seq=20 → `0x20843F37`, seq=22 → `0x8A8DF7BC`.

```python
def esp_crc32_le(data):  # matches esp_rom_crc32_le(UINT32_MAX, data)
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
    return (~c) & 0xFFFFFFFF
```

## Rebuilding from scratch

### 1. IDF tree

The GitHub **source tarball** of `v4.4.7` has empty submodule skeletons; fetch the
pinned submodules via `https://codeload.github.com/<org>/<repo>/tar.gz/<sha>` and
extract into place:

| Submodule path (under IDF root) | Repo | SHA (v4.4.7) |
|---|---|---|
| components/mbedtls/mbedtls | espressif/mbedtls | `2b8e772fc1cb0732cda3bae7d1e9d6f4cfaf63d9` |
| components/lwip/lwip | espressif/esp-lwip | `a45be9e438f6cf9c54ec150581819c3b95d5af6b` |
| components/esp_wifi/lib_esp32 | espressif/esp32-wifi-lib | `f2aae4d44ec7908013066e69d29b9948846c335c` |
| components/esp_phy/lib | espressif/esp-phy-lib | `dcfdccf6cc2fc02d0886624b7998c890d1a19b28` |
| components/tinyusb/tinyusb | hathach/tinyusb | `c4badd394eda18199c0196ed0be1e2d635f0a5f6` |
| components/spiffs/spiffs | pellepl/spiffs | `0dbb3f71c5f6fae3747a9d935372773762baf852` |
| components/json/cJSON | DaveGamble/cJSON | `87d8f0961a01bf09bef98ff89bae9fdec42181ee` |
| components/bootloader/subproject/components/micro-ecc/micro-ecc | kmackay/micro-ecc | `24c60e243580c7868f4334a1ba3123481fe1aa48` |
| components/esptool_py/esptool | espressif/esptool | `7b17ea072fbac0f92fc417ddbe34e28afbd8ced0` |

Exact SHAs can be re-queried from the GitHub API:
`https://api.github.com/repos/espressif/esp-idf/contents/<path>?ref=v4.4.7`.

### 2. Python environment

IDF 4.4 does not install under Python 3.12+ (locked old packages). Use **Python
3.10** (e.g. via `uv python install 3.10`), create a venv, then install IDF
requirements with the poison packages removed: **delete the lines containing
`gdbgui`, `gevent`, `cocotb`, and the `esp-windows-curses` file:// entry** from
`requirements.txt` (the tarball copy may also start with a UTF-8 BOM — strip it).
Verify: `python %IDF_PATH%\tools\check_python_dependencies.py`.

### 3. Configure + build

```
set IDF_PATH=<idf root>
set PATH=<idfenv>\Scripts;<xtensa-esp32s3 toolchain>\bin;<ninja dir>;%PATH%
cmake -G Ninja -DPYTHON_DEPS_CHECKED=1 -DESP_PLATFORM=1 -DIDF_TARGET=esp32s3 ^
      -DCCACHE_ENABLE=0 -S <idfbl> -B <idfbl>\build
ninja -C <idfbl>\build bootloader
```

- **Always pass explicit `-S`/`-B`**: `idf.py` itself configures with
  source == binary and writes build files into the project root, after which
  `ninja -C build` cannot find `build.ninja`.
- `idfbl/CMakeLists.txt` sets `CMAKE_POLICY_VERSION_MINIMUM 3.5` (the mbedtls
  submodule declares `cmake_minimum_required(2.6)`, which modern CMake rejects).
- `sdkconfig.defaults` must contain `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`, or the
  partition-table step fails ("occupies 16.0MB ... does not fit in 2MB").
- Changing sdkconfig.defaults requires deleting the generated `sdkconfig` first
  (existing values take precedence over defaults).

### 4. Key sdkconfig.defaults

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_NUM=0
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
```

## Rescue / unbrick notes

- A crashing firmware in the active slot boot-loops forever (bootloader-level
  rollback is not enabled). Manual rescue: erase the newer otadata sector
  (`erase_region 0xf000 0x1000` — or `0xe000 0x2000` for both) and the bootloader
  falls back to the other slot / OTA 0.
- Reading otadata for diagnosis: entries are at **+0 and +0x1000** (4096 stride),
  not adjacent 32-byte records.
