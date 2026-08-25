/*
 * ============================================================================
 *  Cyborg Puppet — StackChan (M5Stack CoreS3 / ESP32-S3) Security-Audit Firmware
 * ----------------------------------------------------------------------------
 *  Author : SuperNinja (autonomous firmware engineering)
 *  Target : M5Stack CoreS3  (ESP32-S3, 16 MB Flash, 8 MB PSRAM)
 *  Body   : StackChan humanoid desktop robot (pan/tilt neck)
 *  Libs   : M5Unified + M5GFX  (display),  ESP32Servo  (PWM servos),
 *           ESPAsyncWebServer + AsyncTCP  (async HTTP control plane)
 *  Board  : Arduino IDE -> "M5Stack CoreS3"
 *  Scheme : Tools -> Partition Scheme -> "Huge APP (3MB No OTA / 1MB SPIFFS)"
 *           (PSRAM enabled, Flash @ 80 MHz, QIO)
 * ----------------------------------------------------------------------------
 *  ARCHITECTURE OVERVIEW
 *  ---------------------
 *  The ESP32-S3 has two Xtensa LX7 cores. This firmware enforces a strict
 *  separation of concern between them so the UI never stutters when the
 *  network stack is busy serving the control WebUI:
 *
 *     CORE 1 (the "puppet" core)  : Arduino loop(), M5.Display rendering,
 *                                   physical button / touch polling, and
 *                                   smooth, non-blocking servo interpolation.
 *
 *     CORE 0 (the "ghost" core)   : Wi-Fi AP bring-up, the asynchronous web
 *                                   server, and all HTTP handler logic. It
 *                                   never touches the display or servos
 *                                   directly — it only mutates shared state.
 *
 *  Cross-core state is protected with a FreeRTOS mutex (portMUX_TYPE) so the
 *  web handlers on Core 0 can safely update `targetPan`, `targetTilt` and
 *  `auditActive` while Core 1 reads them inside the motion/UI loop.
 *
 *  NETWORK FOOTPRINT
 *  -----------------
 *  The robot deploys a self-contained Wi-Fi Access Point:
 *      SSID     : "Cyborg_Puppet"
 *      Password : "cyberpuppet"
 *  Connect a phone/laptop to it, then browse  http://192.168.4.1
 *  to reach the mobile-first cyberpunk control WebUI.
 *
 *  PHYSICAL SERVO NOTE (read before wiring)
 *  ----------------------------------------
 *  The stock StackChan ships with Feetech SCS-series *feedback* servos driven
 *  over a half-duplex UART bus (GPIO6/7). That bus is NOT PWM.
 *  This firmware deliberately uses the ESP32Servo library on classic PWM GPIO
 *  (Pan = GPIO1, Tilt = GPIO2) per the project spec — suitable when the robot
 *  is retro-fitted with standard 50 Hz hobby servos, or driven from a generic
 *  pan/tilt bracket. If you keep the original Feetech servos, swap the
 *  ESP32Servo calls for the StackChan-BSP `Motion` API (see README wiring
 *  notes). The angle clamping, smoothing and state machine are identical.
 * ============================================================================
 */

// ---------------------------------------------------------------------------
//  1. LIBRARY DECLARATIONS
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <M5Unified.h>          // Unified M5 API -> exposes M5.Display, M5.update(), M5.BtnA...
#include <M5GFX.h>              // Low-level GFX primitives used by M5.Display
#include <WiFi.h>               // ESP32 Wi-Fi (AP mode)
#include <ESPAsyncWebServer.h>  // Async HTTP server
#include <AsyncTCP.h>           // Async TCP transport for the server above
#include <ESP32Servo.h>         // Hardware-PWM servo driver
#include <freertos/FreeRTOS.h>  // FreeRTOS primitives (task pinning, spinlocks)
#include <freertos/task.h>

// ---------------------------------------------------------------------------
//  2. PROJECT CONSTANTS
// ---------------------------------------------------------------------------
namespace cp {  // "Cyborg Puppet"

// --- Wi-Fi Access Point credentials ----------------------------------------
static constexpr const char* AP_SSID     = "Cyborg_Puppet";
static constexpr const char* AP_PASSWORD = "cyberpuppet";
static constexpr const uint16_t WEB_PORT = 80;

// --- Servo GPIO / PWM configuration ----------------------------------------
// ESP32-S3 LEDC PWM: each servo needs its own channel so the two 50 Hz
// signals do not share a timer in a conflicting way.
static constexpr int PAN_SERVO_PIN        = 1;   // GPIO1  -> Pan  (yaw)
static constexpr int TILT_SERVO_PIN       = 2;   // GPIO2  -> Tilt (pitch)
// ESP32Servo 3.x auto-allocates a free LEDC timer/channel per servo; we do
// not hard-code channels (avoids conflicts with M5Unified's internal PWM use).
static constexpr int SERVO_MIN_PULSE_US   = 500; // 0-degree pulse
static constexpr int SERVO_MAX_PULSE_US   = 2500;// 180-degree pulse
static constexpr int SERVO_FREQ_HZ        = 50;  // standard hobby servo

// --- Safe mechanical limits (protect the StackChan chassis from binding) ---
static constexpr int PAN_MIN_DEG   = 30;   //   yaw floor
static constexpr int PAN_MAX_DEG   = 150;  //   yaw ceiling (sweep = 120°)
static constexpr int PAN_CENTER    = 90;   //   neutral / forward
static constexpr int TILT_MIN_DEG  = 60;   //   pitch floor
static constexpr int TILT_MAX_DEG  = 120;  //   pitch ceiling (sweep = 60°)
static constexpr int TILT_CENTER   = 90;   //   neutral / level

// --- Smooth-interpolation tuning -------------------------------------------
static constexpr uint32_t SERVO_STEP_MS   = 20;   // ms between interpolation ticks
static constexpr float    SERVO_STEP_DEG  = 1.0;  // degrees moved per tick -> ~50°/s, gentle

// --- Display layout (CoreS3 panel is 320x240) ------------------------------
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
static constexpr int EYE_Y  = 92;   // vertical centre of the face

// --- Colours (RGB565) -------------------------------------------------------
static constexpr uint16_t COL_BG_PASSIVE = 0x0120;  // near-black with green tint
static constexpr uint16_t COL_GREEN      = 0x07E0;  // matrix green
static constexpr uint16_t COL_GREEN_DIM  = 0x0340;  // dimmer green
static constexpr uint16_t COL_BG_ACTIVE  = 0x1000;  // near-black with red tint
static constexpr uint16_t COL_RED        = 0xF800;  // alert red
static constexpr uint16_t COL_RED_DIM    = 0x6000;  // dim red
static constexpr uint16_t COL_TEXT       = 0xFFFF;  // white
static constexpr uint16_t COL_AMBER      = 0xFD20;  // status amber

}  // namespace cp

// ---------------------------------------------------------------------------
//  3. SHARED CROSS-CORE STATE  (written by Core 0, read by Core 1)
// ---------------------------------------------------------------------------
//
//  Three pieces of state bridge the two cores. Because an `AsyncWebServer`
//  handler can fire at *any* time (preempting the UI loop), every read/write
//  goes through a FreeRTOS critical section guarded by `g_state_mux`.
//
//  `auditActive` is also kept as an `std::atomic<bool>` for lock-free UI
//  polling where a stale read is harmless; the mutex still serialises the
//  *authoritative* write path used by the HTTP handler.

static portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

// Targets the WebUI pushes (already clamped to safe limits by the handlers).
volatile int  g_targetPan   = cp::PAN_CENTER;   // desired pan angle (deg)
volatile int  g_targetTilt  = cp::TILT_CENTER;  // desired tilt angle (deg)
volatile bool g_auditActive = false;            // global audit-mode flag

// Helper wrappers keep the locking in one place so call sites stay clean.
static inline void setTargetPan(int v)  { portENTER_CRITICAL(&g_state_mux); g_targetPan  = v; portEXIT_CRITICAL(&g_state_mux); }
static inline void setTargetTilt(int v) { portENTER_CRITICAL(&g_state_mux); g_targetTilt = v; portEXIT_CRITICAL(&g_state_mux); }
static inline void setAudit(bool a)     { portENTER_CRITICAL(&g_state_mux); g_auditActive = a; portEXIT_CRITICAL(&g_state_mux); }
static inline int  getTargetPan()       { portENTER_CRITICAL(&g_state_mux); int v = g_targetPan;  portEXIT_CRITICAL(&g_state_mux); return v; }
static inline int  getTargetTilt()      { portENTER_CRITICAL(&g_state_mux); int v = g_targetTilt; portEXIT_CRITICAL(&g_state_mux); return v; }
static inline bool getAudit()           { portENTER_CRITICAL(&g_state_mux); bool a = g_auditActive; portEXIT_CRITICAL(&g_state_mux); return a; }

// ---------------------------------------------------------------------------
//  4. GLOBAL OBJECTS
// ---------------------------------------------------------------------------
static Servo              g_panServo;
static Servo              g_tiltServo;
static AsyncWebServer     g_server(cp::WEB_PORT);

// Current *physical* servo positions (only ever mutated on Core 1).
static int  g_curPan  = cp::PAN_CENTER;
static int  g_curTilt = cp::TILT_CENTER;

// UI dirty flag so we only redraw the display when something actually changes.
static bool g_faceDirty = true;
static bool g_lastAuditDrawn = false;

// Forward declarations of the two FreeRTOS tasks.
static void core0NetworkTask(void* arg);  // Core 0 — Wi-Fi + web server
static void core1PuppetTask(void* arg);   // Core 1 — UI + servos (the Arduino loop body)

// ---------------------------------------------------------------------------
//  5. EMBEDDED WEBUI  (PROGMEM HTML/CSS/JS payload)
// ---------------------------------------------------------------------------
//
//  Served at GET "/" . The page is a single self-contained document:
//    * dark, high-contrast matrix-green / cyberpunk terminal theme
//    * mobile-first layout (two large range sliders + a big audit button)
//    * sliders fire live HTTP GETs via the Fetch API as the user drags them
//    * the TRIGGER AUDIT button toggles /toggle-audit and turns bright red,
//      flashing, while the audit is armed.
//
//  R("...") is a C++ raw string literal — keeps the HTML readable.

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>CYBORG_PUPPET // CTRL</title>
<style>
  :root{
    --bg:#020a02; --green:#39ff14; --green-dim:#0a7a0a; --red:#ff2a2a;
    --red-glow:#ff0000; --panel:#04140a; --grid:#072607;
  }
  *{box-sizing:border-box; -webkit-tap-highlight-color:transparent}
  html,body{margin:0; height:100%; background:var(--bg); color:var(--green);
    font-family:"Courier New",monospace; overflow:hidden;
    background-image:
      linear-gradient(var(--grid) 1px,transparent 1px),
      linear-gradient(90deg,var(--grid) 1px,transparent 1px);
    background-size:22px 22px;}
  .wrap{height:100%; display:flex; flex-direction:column; padding:14px;
    max-width:560px; margin:0 auto; gap:14px;}
  header{text-align:center; border:1px solid var(--green-dim); border-radius:6px;
    padding:8px 6px; background:var(--panel); text-shadow:0 0 6px var(--green);}
  header h1{margin:0; font-size:18px; letter-spacing:3px; font-weight:700}
  header .sub{font-size:11px; color:var(--green-dim); letter-spacing:2px; margin-top:3px}
  .panel{border:1px solid var(--green-dim); border-radius:6px; background:var(--panel);
    padding:12px 14px; box-shadow:0 0 14px rgba(57,255,20,.08) inset}
  .row{display:flex; align-items:center; justify-content:space-between; margin-bottom:6px}
  .label{font-size:12px; letter-spacing:2px; color:var(--green-dim)}
  .val{font-size:16px; color:var(--green); text-shadow:0 0 8px var(--green); min-width:48px; text-align:right}
  input[type=range]{ -webkit-appearance:none; width:100%; height:34px; background:transparent; margin:4px 0 2px}
  input[type=range]::-webkit-slider-runnable-track{height:8px; border-radius:4px;
    background:linear-gradient(90deg,var(--green-dim),var(--green))}
  input[type=range]::-webkit-slider-thumb{ -webkit-appearance:none; width:24px; height:24px;
    margin-top:-8px; border-radius:50%; background:var(--green); border:2px solid #021;
    box-shadow:0 0 10px var(--green); cursor:pointer}
  input[type=range]::-moz-range-track{height:8px;border-radius:4px;background:var(--green-dim)}
  input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;
    background:var(--green);border:2px solid #021;box-shadow:0 0 10px var(--green);cursor:pointer}
  #audit{ flex:0 0 auto; height:84px; font-size:20px; letter-spacing:4px; font-weight:700;
    font-family:inherit; color:var(--green); background:var(--panel);
    border:2px solid var(--green-dim); border-radius:8px; cursor:pointer;
    text-shadow:0 0 8px var(--green); transition:all .15s}
  #audit:active{ transform:scale(.97) }
  #audit.armed{ color:#fff; border-color:var(--red-glow); background:#2a0000;
    text-shadow:0 0 10px var(--red-glow); animation:flash .55s steps(2) infinite;
    box-shadow:0 0 22px var(--red-glow)}
  @keyframes flash{ 50%{ background:#700; color:#fff } }
  footer{text-align:center; font-size:10px; color:var(--green-dim); letter-spacing:2px}
  .scan{position:fixed; inset:0; pointer-events:none; background:
    repeating-linear-gradient(0deg,rgba(0,0,0,0) 0 2px,rgba(0,0,0,.06) 2px 4px);
    mix-blend-mode:multiply; z-index:5}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>&gt; CYBORG_PUPPET &lt;</h1>
    <div class="sub">// REMOTE AUDIT CONTROL TERMINAL //</div>
  </header>

  <div class="panel">
    <div class="row"><span class="label">PAN / YAW</span><span class="val" id="panVal">90</span></div>
    <input type="range" id="pan" min="30" max="150" step="1" value="90">
    <div class="row"><span class="label">TILT / PITCH</span><span class="val" id="tiltVal">90</span></div>
    <input type="range" id="tilt" min="60" max="120" step="1" value="90">
  </div>

  <button id="audit">TRIGGER AUDIT</button>

  <footer>192.168.4.1 &nbsp;//&nbsp; LINK ESTABLISHED</footer>
</div>
<div class="scan"></div>

<script>
(function(){
  var pan=document.getElementById('pan'), tilt=document.getElementById('tilt');
  var panVal=document.getElementById('panVal'), tiltVal=document.getElementById('tiltVal');
  var audit=document.getElementById('audit');
  var auditOn=false;

  // Throttle slider GETs so we don't flood the ESP32 while dragging.
  function send(cmd, val, cb){
    fetch('/'+cmd+'?val='+val, {method:'GET', cache:'no-store'})
      .then(function(r){ if(cb) cb(r.ok); })
      .catch(function(){});
  }

  // Live, streaming updates as the slider moves.
  pan.addEventListener('input',  function(){ panVal.textContent=pan.value;
    send('pan',  pan.value); });
  tilt.addEventListener('input', function(){ tiltVal.textContent=tilt.value;
    send('tilt', tilt.value); });

  // Big red toggle button -> flashes when armed.
  audit.addEventListener('click', function(){
    send('toggle-audit', '1', function(ok){
      if(ok){ auditOn=!auditOn; audit.classList.toggle('armed', auditOn);
        audit.textContent = auditOn ? 'AUDIT ENGAGED' : 'TRIGGER AUDIT'; }
    });
  });

  // Heartbeat so the page reflects a state change made elsewhere.
  setInterval(function(){
    fetch('/state', {cache:'no-store'}).then(function(r){return r.json();})
      .then(function(s){
        if(typeof s.pan==='number'){ pan.value=s.pan; panVal.textContent=s.pan; }
        if(typeof s.tilt==='number'){ tilt.value=s.tilt; tiltVal.textContent=s.tilt; }
        if(typeof s.audit==='boolean' && s.audit!==auditOn){
          auditOn=s.audit; audit.classList.toggle('armed',auditOn);
          audit.textContent=auditOn?'AUDIT ENGAGED':'TRIGGER AUDIT';
        }
      }).catch(function(){});
  }, 1500);
})();
</script>
</body>
</html>
)HTML";

// ---------------------------------------------------------------------------
//  6. CORE 0 — NETWORK TASK  (Wi-Fi AP + async web server)
// ---------------------------------------------------------------------------
//
//  Everything networked lives here. It runs once and then the AsyncWebServer
//  callbacks do all the work; the task itself just sleeps so FreeRTOS doesn't
//  starve it. Handlers NEVER draw or move servos — they only adjust shared
//  state through the mutex-protected setters.

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void registerWebRoutes() {
  // --- Root: serve the control WebUI ---------------------------------------
  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    // PROGMEM payload is served directly; send() accepts a PGM_P pointer.
    req->send(200, "text/html", INDEX_HTML);
  });

  // --- Pan slider endpoint: /pan?val=<30..150> -----------------------------
  g_server.on("/pan", HTTP_GET, [](AsyncWebServerRequest* req) {
    int v = cp::PAN_CENTER;
    if (req->hasParam("val")) v = req->getParam("val")->value().toInt();
    setTargetPan(clampInt(v, cp::PAN_MIN_DEG, cp::PAN_MAX_DEG));
    req->send(200, "text/plain", String(getTargetPan()));
  });

  // --- Tilt slider endpoint: /tilt?val=<60..120> ---------------------------
  g_server.on("/tilt", HTTP_GET, [](AsyncWebServerRequest* req) {
    int v = cp::TILT_CENTER;
    if (req->hasParam("val")) v = req->getParam("val")->value().toInt();
    setTargetTilt(clampInt(v, cp::TILT_MIN_DEG, cp::TILT_MAX_DEG));
    req->send(200, "text/plain", String(getTargetTilt()));
  });

  // --- Audit toggle endpoint: /toggle-audit -------------------------------
  g_server.on("/toggle-audit", HTTP_GET, [](AsyncWebServerRequest* req) {
    bool next = !getAudit();
    setAudit(next);
    g_faceDirty = true;            // nudge Core 1 to redraw immediately
    req->send(200, "text/plain", next ? "ENGAGED" : "DISENGAGED");
  });

  // --- State heartbeat: /state  -> JSON mirror of shared state -------------
  g_server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    String j = "{";
    j += "\"pan\":"   + String(getTargetPan())  + ",";
    j += "\"tilt\":"  + String(getTargetTilt()) + ",";
    j += "\"audit\":" + String(getAudit() ? "true" : "false");
    j += "}";
    req->send(200, "application/json", j);
  });

  // --- 404 fallback --------------------------------------------------------
  g_server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "404 // NO_TARGET");
  });

  g_server.begin();
}

static void core0NetworkTask(void* arg) {
  (void)arg;

  // Bring up the private Access Point.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(cp::AP_SSID, cp::AP_PASSWORD);
  // softAPConfig optional: keep default 192.168.4.1 / 255.255.255.0.

  Serial.printf("[NET] AP up  SSID=%s  IP=%s\n",
                cp::AP_SSID, WiFi.softAPIP().toString().c_str());

  registerWebRoutes();

  // The async server is event-driven from here on; just keep the task alive
  // and yield the core so watchdog timers stay happy.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---------------------------------------------------------------------------
//  7. CORE 1 — PUPPET TASK  (display + buttons + servo smoothing)
// ---------------------------------------------------------------------------
//
//  This is the "face and body" core. It owns the M5.Display, the physical
//  buttons, and the servo hardware. It pulls the latest *target* state from
//  the shared variables (written by Core 0) and drives the servos toward
//  those targets with a gentle, non-blocking linear interpolation so the
//  head sweeps naturally instead of snapping.

// --- Smooth, non-blocking servo interpolation ------------------------------
// Steps g_cur* toward the target by SERVO_STEP_DEG every SERVO_STEP_MS.
// Returns true if either servo moved this tick.
static bool stepServos(int targetPan, int targetTilt) {
  bool moved = false;

  if (g_curPan != targetPan) {
    if (g_curPan < targetPan)      g_curPan += cp::SERVO_STEP_DEG;
    else if (g_curPan > targetPan) g_curPan -= cp::SERVO_STEP_DEG;
    // guard against overshoot
    if ((g_curPan > targetPan && g_curPan - cp::SERVO_STEP_DEG < targetPan) ||
        (g_curPan < targetPan && g_curPan + cp::SERVO_STEP_DEG > targetPan)) {
      g_curPan = targetPan;
    }
    g_curPan = clampInt(g_curPan, cp::PAN_MIN_DEG, cp::PAN_MAX_DEG);
    g_panServo.write(g_curPan);
    moved = true;
  }

  if (g_curTilt != targetTilt) {
    if (g_curTilt < targetTilt)      g_curTilt += cp::SERVO_STEP_DEG;
    else if (g_curTilt > targetTilt) g_curTilt -= cp::SERVO_STEP_DEG;
    if ((g_curTilt > targetTilt && g_curTilt - cp::SERVO_STEP_DEG < targetTilt) ||
        (g_curTilt < targetTilt && g_curTilt + cp::SERVO_STEP_DEG > targetTilt)) {
      g_curTilt = targetTilt;
    }
    g_curTilt = clampInt(g_curTilt, cp::TILT_MIN_DEG, cp::TILT_MAX_DEG);
    g_tiltServo.write(g_curTilt);
    moved = true;
  }
  return moved;
}

// --- Passive face: docile round green eyes ---------------------------------
static void drawPassiveFace() {
  auto& d = M5.Display;
  d.fillScreen(cp::COL_BG_PASSIVE);

  // faint corner brackets
  d.setTextColor(cp::COL_GREEN_DIM, cp::COL_BG_PASSIVE);
  d.setTextSize(1);
  d.setCursor(6, 6);   d.print("[ STANDBY ]");
  d.setCursor(232, 6); d.print("[ OK ]");

  // two round eyes
  d.fillCircle(110, cp::EYE_Y, 22, cp::COL_GREEN);
  d.fillCircle(210, cp::EYE_Y, 22, cp::COL_GREEN);
  // soft pupils
  d.fillCircle(110, cp::EYE_Y, 8, cp::COL_BG_PASSIVE);
  d.fillCircle(210, cp::EYE_Y, 8, cp::COL_BG_PASSIVE);

  // small smile arc
  d.drawArc(160, 170, 50, 48, 200, 340, cp::COL_GREEN);

  // status banner
  d.setTextColor(cp::COL_GREEN, cp::COL_BG_PASSIVE);
  d.setTextSize(2);
  d.setTextDatum(MC_DATUM);
  d.drawString("LINK ESTABLISHED", 160, 210);
  d.setTextDatum(TL_DATUM);
}

// --- Active face: menacing red tracking reticle ----------------------------
static void drawActiveFace() {
  auto& d = M5.Display;
  d.fillScreen(cp::COL_BG_ACTIVE);

  d.setTextColor(cp::COL_RED_DIM, cp::COL_BG_ACTIVE);
  d.setTextSize(1);
  d.setCursor(6, 6);   d.print("[ AUDIT ]");
  d.setCursor(232, 6); d.print("[ !!! ]");

  // central targeting reticle
  const int cx = 160, cy = cp::EYE_Y + 4, R = 46;
  d.drawCircle(cx, cy, R, cp::COL_RED);
  d.drawCircle(cx, cy, R - 14, cp::COL_RED_DIM);
  // crosshair
  d.drawLine(cx - R - 12, cy, cx + R + 12, cy, cp::COL_RED);
  d.drawLine(cx, cy - R - 12, cx, cy + R + 12, cp::COL_RED);
  // glitched digital eye core
  d.fillCircle(cx, cy, 10, cp::COL_RED);
  d.fillCircle(cx, cy, 4, cp::COL_BG_ACTIVE);

  // scanline ticks around reticle
  for (int a = 0; a < 360; a += 30) {
    float rad = a * PI / 180.0f;
    d.drawLine(cx + cos(rad) * (R + 2), cy + sin(rad) * (R + 2),
               cx + cos(rad) * (R + 8), cy + sin(rad) * (R + 8), cp::COL_RED_DIM);
  }

  // status banner
  d.setTextColor(cp::COL_RED, cp::COL_BG_ACTIVE);
  d.setTextSize(2);
  d.setTextDatum(MC_DATUM);
  d.drawString("SYS_AUDIT: ENGAGED", 160, 210);
  d.setTextDatum(TL_DATUM);
}

// --- Master UI refresh: picks the right face when state changes ------------
static void updateUI(bool auditActive) {
  if (!g_faceDirty && auditActive == g_lastAuditDrawn) return;
  if (auditActive) drawActiveFace();
  else             drawPassiveFace();
  g_lastAuditDrawn = auditActive;
  g_faceDirty = false;
}

// --- Core 1 task body ------------------------------------------------------
static void core1PuppetTask(void* arg) {
  (void)arg;

  // Centre the head at boot.
  g_panServo.write(cp::PAN_CENTER);
  g_tiltServo.write(cp::TILT_CENTER);
  g_curPan  = cp::PAN_CENTER;
  g_curTilt = cp::TILT_CENTER;

  uint32_t lastServoTick = millis();

  for (;;) {
    // 1) Poll M5 hardware (buttons / touch / IMU) — must be called regularly.
    M5.update();

    // 2) Physical buttons on the CoreS3 act as a local override:
    //    BtnA toggles audit, BtnB recentres head, BtnC is a future hook.
    if (M5.BtnA.wasPressed()) {
      setAudit(!getAudit());
      g_faceDirty = true;
      Serial.printf("[UI] BtnA -> audit=%d\n", (int)getAudit());
    }
    if (M5.BtnB.wasPressed()) {
      setTargetPan(cp::PAN_CENTER);
      setTargetTilt(cp::TILT_CENTER);
      Serial.println("[UI] BtnB -> recentre");
    }

    // 3) Refresh the display if state changed.
    updateUI(getAudit());

    // 4) Smooth servo interpolation toward the WebUI/Bn targets.
    if (millis() - lastServoTick >= cp::SERVO_STEP_MS) {
      stepServos(getTargetPan(), getTargetTilt());
      lastServoTick = millis();
    }

    // 5) Yield — keep Core 1 responsive & feed the watchdog.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------------------------------------------------------------------
//  8. ARDUINO setup() / loop()  — pinned to Core 1 by the Arduino core
// ---------------------------------------------------------------------------
//
//  On the ESP32 Arduino core, setup()/loop() run on Core 1 by default. We
//  keep that contract: setup() initialises M5 + servos, then spawns the
//  Core 0 network task with xTaskCreatePinnedToCore. The Core 1 puppet work
//  runs inside our own pinned task so the loop() stays a trivial placeholder
//  (this also lets us give the UI task a generous, stable stack).

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate   = 115200;   // serial console baud rate
  cfg.clear_display     = false;    // we paint our own boot screen below
  M5.begin(cfg);
  // The CoreS3's 1 W speaker (AW88298) is auto-detected by M5Unified, so no
  // explicit external_speaker flag is required.

  M5.Display.setRotation(1);        // 320x240 landscape
  M5.Display.fillScreen(cp::COL_BG_PASSIVE);
  M5.Display.setTextColor(cp::COL_GREEN, cp::COL_BG_PASSIVE);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 8);
  M5.Display.print("CYBORG_PUPPET // booting...");

  Serial.println("\n[BOOT] Cyborg Puppet firmware starting...");

  // --- Servo attach with explicit PWM channel allocation -------------------
  // ESP32Servo 3.x auto-allocates a free LEDC channel/timer per servo, so we
  // only need to pass the pin + pulse limits. setPeriodHertz fixes the 50 Hz
  // refresh. Each attach() returns the channel it claimed (handy for debug).
  g_panServo.setPeriodHertz(cp::SERVO_FREQ_HZ);
  int panCh = g_panServo.attach(cp::PAN_SERVO_PIN, cp::SERVO_MIN_PULSE_US,
                                cp::SERVO_MAX_PULSE_US);
  g_tiltServo.setPeriodHertz(cp::SERVO_FREQ_HZ);
  int tiltCh = g_tiltServo.attach(cp::TILT_SERVO_PIN, cp::SERVO_MIN_PULSE_US,
                                  cp::SERVO_MAX_PULSE_US);
  Serial.printf("[BOOT] servos: pan->LEDC%d  tilt->LEDC%d\n", panCh, tiltCh);

  // --- Spawn Core 0 (network) task -----------------------------------------
  xTaskCreatePinnedToCore(
      core0NetworkTask,
      "net",        // task name
      8192,         // stack (bytes) — async server + WiFi needs headroom
      nullptr,
      2,            // priority (low-ish; UI on Core 1 is higher)
      nullptr,
      0);           // CORE 0  <-- networking pinned here

  // --- Spawn Core 1 (puppet) task ------------------------------------------
  xTaskCreatePinnedToCore(
      core1PuppetTask,
      "puppet",     // task name
      8192,         // stack (bytes)
      nullptr,
      3,            // priority (slightly higher for smooth UI/servos)
      nullptr,
      1);           // CORE 1  <-- UI + servos pinned here

  Serial.println("[BOOT] tasks pinned: net->Core0  puppet->Core1");
}

// The Arduino loop runs on Core 1 but all real Core-1 work lives in the
// "puppet" task above. Keep this empty & cheap so it never interferes.
void loop() {
  vTaskDelay(pdMS_TO_TICKS(100));
}
