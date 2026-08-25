# Reproducible build (arduino-cli)

This is the exact sequence used to produce the pre-built binaries in
`CyborgPuppet/build/`.

## 1. Install arduino-cli

```bash
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/usr/local/bin sh
```

## 2. Configure the ESP32 board index

```bash
arduino-cli config init --overwrite
arduino-cli config set board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

## 3. Install libraries

```bash
arduino-cli lib install "M5Unified" "M5GFX" "ESP32Servo"
arduino-cli lib install "ESP Async WebServer@3.12.0"   # ESP32Async fork
arduino-cli lib install "Async TCP@3.3.8"              # ESP32Async fork (matched)
```

> Do **not** use the legacy `me-no-dev/ESPAsyncWebServer` — it fails to
> compile against arduino-esp32 3.x (removed `mbedtls_md5_*_ret`).

## 4. Compile

```bash
FQBN="esp32:esp32:m5stack_cores3:UploadSpeed=921600,FlashMode=qio,FlashSize=16M,PartitionScheme=huge_app,PSRAM=enabled,CPUFreq=240"

arduino-cli compile \
  --fqbn "$FQBN" \
  --warnings default \
  --output-dir CyborgPuppet/build \
  CyborgPuppet
```

### Expected output

```
Sketch uses 1111367 bytes (35%) of program storage space. Maximum is 3145728 bytes.
Global variables use 48980 bytes (14%) of dynamic memory, leaving 278700 bytes for local variables.
```

## 5. Artifacts

After a successful build, `CyborgPuppet/build/` contains:

| File                                | Use                                   |
|-------------------------------------|---------------------------------------|
| `CyborgPuppet.ino.merged.bin`       | Flash at `0x0` (easiest — all-in-one) |
| `CyborgPuppet.ino.bin`              | App only, flash at `0x10000`          |
| `CyborgPuppet.ino.bootloader.bin`   | Flash at `0x0`                        |
| `CyborgPuppet.ino.partitions.bin`   | Flash at `0x8000`                     |

The `--flash-mode dio --flash-freq 80m --flash-size 16MB` flags (see
`build/esp32.esp32.m5stack_cores3/flash_args`) describe the flash geometry.

## Flashing on Android (esp32_flasher)

Transfer `CyborgPuppet.ino.merged.bin` to the phone, open esp32_flasher, select
the file, address `0x0`, pick the CoreS3 USB-CDC serial port, and flash.
