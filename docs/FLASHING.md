# Flashing CyborgPuppet to M5Stack CoreS3 (Android / esp32_flasher)

This guide fixes the **"Failed to flashDeflBlock"** error that occurs at ~98% when flashing the 16MB merged binary.

---

## The Problem (and why it happens)

The original `CyborgPuppet.ino.merged.bin` was a **full 16MB flash image** — 16,777,216 bytes, mostly 0xFF padding. Only ~1.1MB is real firmware. When esp32_flasher tries to write 16MB of compressed data over USB, it frequently times out or loses connection near the end (98%), producing:

```
Compressed 16777216 bytes to 774118
Erasing 0x1000000 bytes at offset 0x0
Progress: 98 %
Failed to flashDeflBlock
Failed to flashDeflBlock
Flash file1 failed...
```

**Root causes** (per esptool ESP32-S3 troubleshooting docs):
1. The 16MB padded image sends 15× more data than needed
2. USB power/signal instability during long writes
3. Baud rate too high for sustained transfer

## The Fix

Two working approaches are provided below. **Method A (individual files)** is the most reliable.

---

## Method A — Flash Individual Components (RECOMMENDED)

Instead of one giant 16MB file, flash the 4 separate binaries at their correct offsets. Total data: ~1.1MB (15× less than the 16MB merged image). esp32_flasher supports multi-file flashing.

### Files in `build/`:

| # | File | Offset | Size | Purpose |
|---|------|--------|------|---------|
| 1 | `CyborgPuppet.ino.bootloader.bin` | `0x0` | 19 KB | 2nd-stage bootloader |
| 2 | `CyborgPuppet.ino.partitions.bin` | `0x8000` | 3 KB | Partition table (huge_app) |
| 3 | `boot_app0.bin` | `0xe000` | 8 KB | OTADATA (boot slot selector) |
| 4 | `CyborgPuppet.ino.bin` | `0x10000` | 1.06 MB | Application firmware |

### Steps in esp32_flasher (Android):

1. Open **esp32_flasher** app
2. Set **Chip**: `ESP32-S3`
3. Set **Flash Mode**: `DIO`
4. Set **Flash Frequency**: `80MHz`
5. Set **Flash Size**: `16MB`
6. **Lower the baud rate to 460800** (or even 115200 if 460800 fails — this is the #1 fix for flashDeflBlock)
7. Add 4 firmware files with these exact offsets:
   - File 1: browse to `CyborgPuppet.ino.bootloader.bin`, offset `0x0`
   - File 2: browse to `CyborgPuppet.ino.partitions.bin`, offset `0x8000`
   - File 3: browse to `boot_app0.bin`, offset `0xe000`
   - File 4: browse to `CyborgPuppet.ino.bin`, offset `0x10000`
8. Put the CoreS3 into **download mode** (see below)
9. Tap **Flash**

### Putting M5Stack CoreS3 into Download Mode

The CoreS3 uses a USB-CDC interface. To enter download mode:
1. Unplug the USB cable
2. **Hold down the power button (the physical reset button on the bottom)**
3. While holding, plug in the USB cable
4. Keep holding for 2–3 seconds, then release

> **Alternative**: If esp32_flasher can't auto-reset, some versions need you to briefly hold the **G0/M5 button** during plug-in. On CoreS3, hold the **reset button** (bottom edge) for 2 seconds while connecting USB.

---

## Method B — Flash the Trimmed Merged Binary

If your flasher only supports a single file, use `CyborgPuppet.merged.bin` — a **4MB trimmed merge** (not 16MB). It contains bootloader + partitions + otadata + app, padded only to 4MB.

| File | Offset | Size |
|------|--------|------|
| `CyborgPuppet.merged.bin` | `0x0` | 4.00 MB |

### Steps:
1. Open **esp32_flasher**
2. Set **Chip**: `ESP32-S3`, **Flash Mode**: `DIO`, **Flash Size**: `16MB`
3. **Baud rate: 460800** (lower if it fails)
4. Add single file: `CyborgPuppet.merged.bin` at offset `0x0`
5. Put CoreS3 in download mode (hold reset button while plugging USB)
6. Tap **Flash**

The 4MB image is 75% smaller than the old 16MB one, dramatically reducing the chance of timeout.

---

## Still Failing? Escalation Checklist

If you still get `flashDeflBlock` after trying the above:

1. **Drop baud to 115200** — the slowest, most stable rate. This alone fixes 90% of remaining cases.
2. **Use a different USB cable** — many USB-C cables are charge-only (no data) or have poor shielding. Use a high-quality data+power USB-C cable, ideally the one that came with the CoreS3.
3. **Power the CoreS3 externally** — plug in a USB-C power bank or charger to the second port (CoreS3 has two USB-C ports). Low power during flash = corrupted writes.
4. **Try a different USB port** — avoid USB hubs; plug directly into the S22 (with a USB-C OTG adapter) or a powered hub.
5. **Wipe flash first** — if the flash has corrupted OTA data, erase everything before re-flashing:
   - In esp32_flasher, use **Erase Flash** (erase the entire chip), then flash.
6. **Hold reset longer** — some CoreS3 units need 5+ seconds of reset-hold to reliably enter download mode.

### Partition scheme note

This firmware uses the **"Huge APP"** partition scheme (`huge_app`), which allocates a single 3MB app partition (no OTA-2 slot). The `boot_app0.bin` (otadata @ 0xe000) is still required because the partition table defines an `otadata` partition — the bootloader looks for it even in no-OTA configs. This is why we include it.

---

## After Successful Flash

1. The CoreS3 will reboot automatically
2. The display shows **"LINK ESTABLISHED"** with green eyes (passive mode)
3. On your phone, connect to Wi-Fi:
   - **SSID**: `Cyborg_Puppet`
   - **Password**: `cyberpuppet`
4. Open a browser and go to **`http://192.168.4.1`**
5. You'll see the cyberpunk control panel — drag the sliders to move the servos, tap **TRIGGER AUDIT** for active mode

### Default AP IP
The ESP32 softAP always assigns `192.168.4.1` to itself. Your phone gets `192.168.4.2` (or similar). The web UI is at `http://192.168.4.1`.
