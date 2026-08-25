# Wiring Guide

Two servo options are supported. Pick the one that matches your hardware.

---

## Option 1 — Standard PWM hobby servos (matches the firmware as-shipped)

The firmware uses the **ESP32Servo** library on classic 50 Hz PWM GPIO:

| Servo | ESP32-S3 GPIO | Wire colour (typical) |
|-------|---------------|-----------------------|
| Pan   | **GPIO 1**    | signal (orange)       |
| Tilt  | **GPIO 2**    | signal (orange)       |

Power the servos from a **separate 5 V supply** (not the CoreS3's 3.3 V rail —
servo current spikes will brown out the ESP32). Tie all grounds together
(ESP32 GND ↔ servo supply GND).

```
   5V servo supply  ──►  servo VCC (red)
   GND (shared)     ──►  servo GND (brown)
   GPIO1            ──►  Pan  servo signal (orange)
   GPIO2            ──►  Tilt servo signal (orange)
```

If GPIO 1/2 are already used by another StackChan module in your build, edit
the constants at the top of `CyborgPuppet.ino`:

```cpp
static constexpr int PAN_SERVO_PIN  = 1;
static constexpr int TILT_SERVO_PIN = 2;
```

Any **output-capable** GPIO on the ESP32-S3 works (avoid strapping pins
GPIO0/45/46 and the flash pins 26-32). Good alternatives: GPIO 4, 5, 6, 7,
13-18, 38-42.

---

## Option 2 — Stock StackChan Feetech SCS feedback servos

The stock robot uses **Feetech SCS-series** servos on a half-duplex UART bus
(routed through the StackChan body's M-BUS), **not** PWM. To drive them with
this firmware, swap the `ESP32Servo` calls for the StackChan-BSP Motion API.

### Required libraries (add on top of the existing ones)

* **StackChan-BSP** — https://github.com/m5stack/StackChan-BSP
* (its deps: `IRremoteESP8266`, `M5Unit-NFC`)

### Code changes

Replace the servo attach block in `setup()` and the `stepServos()` writes:

```cpp
#include <StackChan_BSP.h>          // provides m5::M5StackChan  + M5StackChan

// in setup(), after M5.begin():
M5StackChan.begin();
M5StackChan.setServoPowerEnabled(true);

// in stepServos(), replace g_panServo.write()/g_tiltServo.write():
//   yaw   = pan  (horizontal),  range 0..100 mapped from PAN_MIN..PAN_MAX
//   pitch = tilt (vertical),    range 0..900 mapped from TILT_MIN..TILT_MAX
M5StackChan.Motion.moveYaw(
    map(g_curPan,  cp::PAN_MIN_DEG,  cp::PAN_MAX_DEG,  0,   1000));
M5StackChan.Motion.movePitch(
    map(g_curTilt, cp::TILT_MIN_DEG, cp::TILT_MAX_DEG, 0,   900));
```

The angle clamping, smoothing interpolation, cross-core state and UI logic are
**unchanged** — only the physical-write layer is swapped.

### Stock servo bus pins (CoreS3 side)

| Function      | GPIO |
|---------------|------|
| Servo UART TX | 7    |
| Servo UART RX | 6    |

These are fixed by the StackChan-BSP `servo_init()` (1 Mbaud half-duplex).

---

## Pan/tilt axis reference

```
            TILT 90° (level / forward)
                 ▲
        ┌────────┼────────┐
        │   ↑    │   ↑    │
  TILT 120°      │      (looks up)
        │        │        │
        └────────┼────────┘

   ◄── PAN 30°   PAN 90° (center)   PAN 150° ──►
   (look left)   (forward)          (look right)
```

All targets sent by the WebUI are clamped to these ranges by the HTTP handlers
before they reach the shared state, so even a malicious slider value cannot
command the servos past the safe mechanical limits.
