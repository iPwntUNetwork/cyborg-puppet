# Cyborg Puppet 🤖🔌

**Interactive, remote-controlled "security-auditing" firmware for the M5Stack StackChan robot (CoreS3 / ESP32-S3).**

StackChan becomes a covert Wi-Fi access point. You connect your phone, open a
mobile-first cyberpunk control panel, and drive the robot's pan/tilt neck with
two sliders while toggling an "audit mode" that flips its on-screen face from a
docile green stare to a menacing red tracking reticle.

The firmware enforces a strict **dual-core separation of concern** so the UI
and servos never stutter while the async web server is serving requests.

---

## Architecture

```
                ESP32-S3 (dual-core Xtensa LX7 @ 240 MHz)
 ┌────────────────────────────────┬─────────────────────────────────┐
 │            CORE 1 (puppet)     │           CORE 0 (ghost)        │
 │  Arduino loop() / "puppet" task│   "net" task                    │
 │                                │                                 │
 │  • M5.Display face rendering   │  • Wi-Fi softAP bring-up         │
 │  • Physical button polling     │  • AsyncWebServer (port 80)      │
 │  • Smooth servo interpolation  │  • HTTP handlers (/pan,/tilt,…)  │
 │                                │  • Mutates shared state only     │
 │  Reads: targetPan/Tilt,        │  Writes: targetPan/Tilt,         │
 │         auditActive            │         auditActive              │
 └─────────────────┬──────────────┴──────────────────┬───────────────┘
                   │   portMUX_TYPE critical section  │
                   └──────────────┬───────────────────┘
                                  ▼
                shared state: targetPan, targetTilt, auditActive
```

* **Core 1** owns the display, buttons and servo hardware and runs a gentle,
  non-blocking linear interpolation toward the requested angles so the head
  sweeps naturally (~50°/s) instead of snapping.
* **Core 0** owns Wi-Fi and the async web server. Handlers **never** draw or
  move servos directly — they only update the mutex-protected shared state.
* Cross-core state is guarded by a FreeRTOS `portMUX_TYPE` spinlock, so an
  async HTTP handler firing mid-frame cannot corrupt the UI/servo reads.

---

## Network footprint

The robot deploys a self-contained Wi-Fi Access Point (no router needed):

| Setting  | Value             |
|----------|-------------------|
| SSID     | `Cyborg_Puppet`   |
| Password | `cyberpuppet`     |
| Gateway  | `192.168.4.1`     |
| Web UI   | `http://192.168.4.1` |

Connect a phone or laptop to the AP, then browse to the gateway.

### HTTP endpoints

| Endpoint            | Method | Purpose                                       |
|---------------------|--------|-----------------------------------------------|
| `/`                 | GET    | Serves the embedded `index_html` control UI   |
| `/pan?val=30..150`  | GET    | Set target pan (clamped to safe range)        |
| `/tilt?val=60..120` | GET    | Set target tilt (clamped to safe range)       |
| `/toggle-audit`     | GET    | Toggle audit-active state                     |
| `/state`            | GET    | JSON heartbeat: `{pan,tilt,audit}`            |

---

## WebUI

A single self-contained HTML/CSS/JS document stored in `PROGMEM`:

* Dark, high-contrast **matrix-green / cyberpunk terminal** theme with a CRT
  scanline overlay.
* Mobile-first layout: two large range sliders + a big audit button.
* Sliders stream live `GET` requests via the **Fetch API** as you drag them.
* The **TRIGGER AUDIT** button turns bright red and flashes while armed.
* A `/state` heartbeat keeps the page in sync if state changes elsewhere
  (e.g. via the physical buttons).

---

## Robot UI states

| State                  | Display                                                         |
|------------------------|-----------------------------------------------------------------|
| **PASSIVE** (idle)     | Green round eyes + smile, banner `LINK ESTABLISHED`             |
| **ACTIVE AUDIT**       | Red tracking reticle + crosshair + glitched eye, banner `SYS_AUDIT: ENGAGED` |

Local override buttons (CoreS3 front panel):
* **BtnA** → toggle audit mode
* **BtnB** → re-centre head (pan/tilt to 90°)

---

## Servo / mechanical limits

Designed to avoid structural binding on the StackChan chassis:

| Servo | GPIO | Min   | Max    | Center | Sweep |
|-------|------|-------|--------|--------|-------|
| Pan   | 1    | 30°   | 150°   | 90°    | 120°  |
| Tilt  | 2    | 60°   | 120°   | 90°    | 60°   |

Smooth interpolation: `1°` per `20 ms` tick (~50°/s).

> ⚠️ **Hardware note — read before wiring.** The stock StackChan ships with
> **Feetech SCS-series feedback servos** driven over a half-duplex UART bus
> (GPIO6/7), *not* classic PWM servos. This firmware uses the **ESP32Servo**
> library on PWM GPIO 1 & 2 per the project spec, which suits a retrofit with
> standard 50 Hz hobby servos or a generic pan/tilt bracket.
>
> If you keep the original Feetech servos, replace the `ESP32Servo` calls with
> the StackChan-BSP `m5::M5StackChan::Motion` API (`moveYaw`/`movePitch`).
> The angle clamping, smoothing state machine and UI logic are unchanged.
> See `docs/WIRING.md` for both wiring options.

---

## Required libraries (Arduino IDE Library Manager)

| Library              | Version  | Purpose                          |
|----------------------|----------|----------------------------------|
| **M5Unified**        | 0.2.20+  | Unified M5 API (`M5.Display`)    |
| **M5GFX**            | 0.2.28+  | GFX primitives (dep of M5Unified)|
| **ESP32Servo**       | 3.2.1+   | PWM servo driver                 |
| **ESP Async WebServer** (ESP32Async fork) | 3.12.0+ | Async HTTP server |
| **Async TCP** (ESP32Async fork)           | 3.3.8+  | Async transport   |

> The original `me-no-dev/ESPAsyncWebServer` is **incompatible** with
> arduino-esp32 3.x (removed `mbedtls_md5_*_ret` symbols). Use the ESP32Async
> forks listed above, which are maintained for the 3.x core.

## Board / IDE settings

* **Board:** `M5Stack CoreS3`  (FQBN `esp32:esp32:m5stack_cores3`)
* **Partition Scheme:** `Huge APP (3MB No OTA/1MB SPIFFS)`
* **PSRAM:** `Enabled` (QSPI)
* **Flash Size:** `16MB` · **Flash Mode:** `QIO 80MHz` · **CPU:** `240 MHz`
* **Upload Speed:** `921600` (or `1500000`)

Full FQBN used for the verified build:
```
esp32:esp32:m5stack_cores3:UploadSpeed=921600,FlashMode=qio,FlashSize=16M,PartitionScheme=huge_app,PSRAM=enabled,CPUFreq=240
```

---

## Flashing

> **See [`docs/FLASHING.md`](docs/FLASHING.md) for the full step-by-step guide,
> including how to fix the "Failed to flashDeflBlock" error.**

### ⚠️ Important — read if you got "Failed to flashDeflBlock" at 98%

The original 16MB merged binary caused flashing timeouts because it was a
full-flash image padded with 0xFF. It has been **replaced with a 4MB trimmed
merge** and individual component binaries. Use Method A below.

### Method A — Individual components (RECOMMENDED — most reliable)

Flash the 4 separate binaries at their offsets. Total ~1.1MB (vs 16MB). In
esp32_flasher, set **Chip: ESP32-S3**, **Flash Mode: DIO**, **Flash Size:
16MB**, **Baud: 460800** (drop to 115200 if it fails), then add:

| # | File                                 | Offset    | Purpose              |
|---|--------------------------------------|-----------|----------------------|
| 1 | `CyborgPuppet.ino.bootloader.bin`    | `0x0`     | Bootloader           |
| 2 | `CyborgPuppet.ino.partitions.bin`    | `0x8000`  | Partition table      |
| 3 | `boot_app0.bin`                      | `0xe000`  | OTADATA              |
| 4 | `CyborgPuppet.ino.bin`               | `0x10000` | Application firmware |

Put the CoreS3 in download mode (hold the reset button while plugging USB),
then tap Flash.

### Method B — Single trimmed merged binary

If your flasher only supports one file, use the **4MB trimmed merge**:

```
CyborgPuppet/build/CyborgPuppet.merged.bin   @ 0x0
```

This contains all 4 components in one 4MB file (75% smaller than the old 16MB
image). Flash at offset `0x0`.

### Method C — Arduino IDE

Open `CyborgPuppet/CyborgPuppet.ino`, select the board/options above, click
Upload.

---

## Repository layout

```
CyborgPuppet/
├── CyborgPuppet.ino          # the firmware (single file, heavily commented)
├── build/                    # pre-built firmware binaries (verified compile)
│   ├── CyborgPuppet.merged.bin         # ← 4MB trimmed merge, flash @ 0x0
│   ├── CyborgPuppet.ino.bin            # app-only @ 0x10000
│   ├── CyborgPuppet.ino.bootloader.bin # bootloader @ 0x0
│   ├── CyborgPuppet.ino.partitions.bin # partition table @ 0x8000
│   ├── boot_app0.bin                   # otadata @ 0xe000
│   └── flash_manifest.json             # esp32_flasher manifest
└── docs/
    ├── FLASHING.md           # ★ flashing guide + flashDeflBlock fix
    ├── WIRING.md             # servo wiring options (PWM vs Feetech)
    └── BUILD.md              # reproducible arduino-cli build instructions
```

## Verified build

Compiled successfully with arduino-cli 1.5.1 + arduino-esp32 3.3.11:

```
Sketch uses 1,111,367 bytes (35%) of program storage space. (max 3,145,728)
Global variables use 48,980 bytes (14%) of dynamic memory.   (max 327,680)
```

## Safety / responsible use

This firmware turns a cute robot into a Wi-Fi AP with a controllable head and a
themed "audit" UI. It is a **teaching / CTF-lab / home-lab** tool. Use it only
on networks and devices you own or are explicitly authorized to test.

## License

MIT — see `LICENSE`.
