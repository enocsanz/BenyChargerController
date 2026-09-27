#include "BenyTask.h"
#include "EsiosTask.h"
#include "GoogleSheetsTask.h"
#include "HuaweiTask.h"
#include "TelegramTask.h"
#include "TermoTask.h"
#include "PiscinaTask.h"

#include "config.h"
#include <Arduino.h>
#include <M5Dial.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

// WDT Timeout (seconds)
#define WDT_TIMEOUT 30

// Logic Constants
// Dead band around the DLB target, to avoid amp jitter (W)
const int32_t GRID_DEADBAND = 200;
// Resend the setpoint at most this often when the charger does not follow it
const unsigned long DLB_RESYNC_INTERVAL = 10000;
// ...and sooner when it draws more than asked
const unsigned long DLB_RESYNC_OVER = 3000;

// Queues a message for the Telegram task. Never blocks: the actual HTTPS
// request is issued from loopTelegram().
extern void sendTelegramNotification(String msg);

// Global State
unsigned long lastLogicRun = 0;
const unsigned long logicInterval = 1000; // Check every 1 second (Faster DLB)

// --- M5Dial UI ---
// Round 240x240 GC9A01. Everything is drawn into an off-screen canvas and
// pushed in one go, so the 0.5s refresh does not flicker. 8-bit colour keeps
// it at ~57KB of RAM (the StampS3A has no PSRAM and TLS needs its share).
M5Canvas canvas(&M5Dial.Display);
long lastEncoderPos = 0;

// Boot log: a few centred lines, since the round screen has no usable corners
int bootLine = 0;
void bootMsg(const String &msg, uint16_t color = TFT_WHITE) {
  M5Dial.Display.setTextColor(color, TFT_BLACK);
  M5Dial.Display.drawCenterString(msg, 120, 50 + bootLine * 24);
  bootLine++;
}

#include <Preferences.h>

Preferences preferences;

// Charging Algorithm Globals - Simplified
// Modes: 0=SOLAR, 1=BALANCEO
int charging_mode = 0;                       // Default: 0 (SOLAR)
int max_grid_power = DEFAULT_MAX_GRID_POWER; // Default from config
float max_price_threshold = PRICE_THRESHOLD; // Info only
bool manual_logic_trigger = false;           // Trigger for immediate logic run
int target_amps = BENY_MIN_AMPS;             // Start conservatively

// Screen Dimming State
unsigned long lastInteractionTime = 0;
const unsigned long SCREEN_TIMEOUT = 120000; // 2 minutes
bool screenAwake = true;

void saveMode(int mode) {
  preferences.begin("beny", false);
  preferences.putInt("mode", mode);
  preferences.end();
  Serial.printf("Saved Mode: %d\n", mode);
}

void saveMaxGridPower(int watts) {
  preferences.begin("beny", false);
  preferences.putInt("limit", watts);
  preferences.end();
  Serial.printf("Saved Grid Limit: %d\n", watts);
}

void setup() {
  // M5Unified detects the Dial and holds GPIO46 high (power latch) by itself.
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false); // encoder ON, RFID OFF
  M5Dial.Display.setBrightness(128);

  // WDT Init
  esp_task_wdt_init(WDT_TIMEOUT, true); // Enable panic (reset) on timeout
  esp_task_wdt_add(NULL);               // Add current thread to WDT

  // recover mode and configs
  preferences.begin("beny", true);               // Read only
  charging_mode = preferences.getInt("mode", 0); // Default 0
  max_grid_power = preferences.getInt("limit", DEFAULT_MAX_GRID_POWER);
  preferences.end();
  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setFont(&fonts::FreeSansBold9pt7b);
  bootMsg("Beny DLB - M5Dial");

  canvas.setColorDepth(8);
  canvas.createSprite(240, 240);
  lastEncoderPos = M5Dial.Encoder.read();

  Serial.begin(115200);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Modem sleep adds latency to the Beny UDP / Modbus polls
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    esp_task_wdt_reset(); // Prevent WDT reset while connecting
  }

  if (WiFi.status() == WL_CONNECTED) {
    bootMsg("WiFi OK", TFT_GREEN);
  } else {
    // Do NOT reboot here. Rebooting on a failed boot-time connect turned a
    // router outage into an endless boot loop (20s of dots -> restart -> 20s
    // of dots...), which is what the "black screen filling with dots" is.
    // Boot anyway and let loop() own reconnection.
    bootMsg("WiFi: SIN RED", TFT_RED);
    Serial.println("WiFi: Sin conexion al arrancar. Continuando, loop() reintentara.");
  }

  // Configure Time — proper DST handling for Spain (CET/CEST).
  // Safe to call without a link: the SNTP client syncs on its own once the
  // network comes up, so a boot without WiFi still gets the time later.
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  if (WiFi.status() == WL_CONNECTED) {
    struct tm timeinfo;
    unsigned long ntpStart = millis();
    bool timeSet = false;
    while (!(timeSet = getLocalTime(&timeinfo, 1000))) {
      if (millis() - ntpStart > 10000) {
        Serial.println("\nNTP: Timeout, continuing without correct time.");
        bootMsg("Hora: FALLO", TFT_RED);
        break;
      }
      esp_task_wdt_reset(); // Prevent WDT reset during NTP sync
    }
    if (timeSet) bootMsg("Hora OK", TFT_GREEN);
  }

  // Init Tasks
  setupTelegram();
  setupGoogleSheets();

  setupBeny();

  setupHuawei();
  // setupWeather(); // Init weather (fetch forecast) REMOVED

  setupEsios(); // Fetches price immediately

  setupTermo();
  setupPiscina();

  // Force Logic run immediately
  lastLogicRun = millis() - logicInterval;

  String modeStr = (charging_mode == 0) ? "SOLAR" : "BALANCEO";
  sendTelegramNotification("🚀 Sistema Iniciado. Modo actual: " + modeStr);

  // Why we booted: a watchdog or panic reset here is worth knowing about
  static const char *resetNames[] = {"desconocido", "encendido", "externo", "software",
                                     "panic",       "int_wdt",   "task_wdt", "wdt",
                                     "deepsleep",   "brownout",  "sdio"};
  int rr = (int)esp_reset_reason();
  logEvent("ARRANQUE", String("Reinicio: ") +
                           (rr >= 0 && rr <= 10 ? resetNames[rr] : "?") + ", modo " + modeStr);
}

void runSmartChargingLogic() {
  // PURE Dynamic Load Balancing (DLB) Logic
  // Goal: Keep grid power at the mode's target by modulating the charge current
  // Action: Adjust Amps (BENY_MIN_AMPS - BENY_MAX_AMPS) on each fresh grid sample
  //
  // There is NO automatic pause: the charger runs in "Plug and Charge" and
  // ignores a STOP (the car restarts it, and on that self-restart it uses the
  // charger's own max current instead of our setpoint), so a STOP-based pause
  // ended up charging at FULL power - the exact opposite of its purpose.
  // The floor is therefore BENY_MIN_AMPS, which the charger does honour.

  BenyData bd = getBenyData();

  // 1. CHECK STATUS:
  if (bd.status == "DISCONNECTED" || bd.status == "UNPLUGGED") {
    return;
  }

  // 2. AUTO-START: If waiting/standby, tell it to start
  if (bd.status == "WAITING" || bd.status == "STANDBY") {
    static unsigned long lastStartAttempt = 0;
    if (millis() - lastStartAttempt > 5000) { // Don't spam start commands
      lastStartAttempt = millis();
      benyStartCharge();
      Serial.println("Auto-Start: El cargador estaba en espera. Enviando orden de inicio.");
    }
    return; // Wait for it to change status to CHARGING
  }

  if (bd.status != "CHARGING" && bd.status != "STARTING") {
    return;
  }

  // --- DLB: act only on a fresh grid sample ---
  // The grid reading lags (1s, or 10s when the inverter is slow) and so does
  // the car, which takes seconds to follow a new setpoint. Stepping every
  // second on a repeated reading piled up adjustments and made the grid swing
  // between ~2.7kW and ~5.9kW around a 4.6kW limit.
  static uint32_t lastSample = 0;
  if (grid_sample_count == lastSample) {
    return;
  }
  lastSample = grid_sample_count;

  int32_t limit_watts = (charging_mode == 0) ? SOLAR_GRID_TARGET : max_grid_power;
  int32_t error_watts = limit_watts - current_grid_power; // > 0: room left
  int ideal_amps = target_amps;

  if (error_watts < -GRID_DEADBAND) {
    // Over the limit: step down 1A per fresh sample, and only once the car
    // has followed the previous step (otherwise the steps pile up and it
    // drops further than needed). A single jump to the computed value was
    // too abrupt, and not needed: the distributor tolerates minutes over the
    // limit and the installation is sized well above the contracted power.
    // Always from the setpoint: the measured current lags while the car is
    // still climbing, and stepping from it made jumps like 19 -> 13A.
    if (bd.current <= target_amps + 1) ideal_amps = target_amps - 1;
  } else if (error_watts > GRID_DEADBAND) {
    // Room left: 1A up per fresh sample, and only once the car has caught up
    // with the previous setpoint (it may also be limiting itself, e.g. near
    // full). 2A steps overshot and hunted with the house loads swinging.
    if (bd.current >= target_amps - 1) ideal_amps = target_amps + 1;
  }

  // 4. CLAMPING
  if (ideal_amps < BENY_MIN_AMPS) ideal_amps = BENY_MIN_AMPS;
  if (ideal_amps > BENY_MAX_AMPS) ideal_amps = BENY_MAX_AMPS;

  // 5. ACTUATION & SYNC
  // Resend the setpoint if the charger reports a very different current (a
  // lost UDP packet, or a self-restart at its own max), but not every second.
  // Drawing MORE than asked is the dangerous side (seen: 27A with 19A asked,
  // after the car restarted its session), so that one is resent sooner.
  static unsigned long lastSync = 0;
  unsigned long sinceSync = millis() - lastSync;
  bool sync_needed = (bd.current > target_amps + 2 && sinceSync > DLB_RESYNC_OVER) ||
                     (abs(bd.current - target_amps) > 2 && sinceSync > DLB_RESYNC_INTERVAL);

  if (ideal_amps != target_amps || sync_needed) {
    Serial.printf("DLB: Grid %d, Limit %d | Adjusting %d -> %dA (Phys: %.1fA)\n",
                  current_grid_power, limit_watts, target_amps, ideal_amps, bd.current);
    target_amps = ideal_amps;
    benySetCurrent(target_amps);
    lastSync = millis();
  }
}

// --- UI Logic ---
bool redraw = true;

// Round screen layout (240x240). Rows are centred; the usable width shrinks
// towards the top and bottom edges, so the outer rows carry the shortest text.
// Same colour rules as the M5StickC Plus version.
void drawStatusScreen(bool fullClear) {
  (void)fullClear; // The canvas is always redrawn from scratch
  if (!screenAwake) return; // Nothing to show while the backlight is off

  canvas.fillSprite(TFT_BLACK);
  canvas.setTextDatum(middle_center);
  char buf[40];
  uint16_t c;

  // Water heater, top row (the narrowest: at most ~9 characters).
  // Cyan heating, green enabled, orange cut by price, red cut by overload.
  TermoStatus ts = getTermoStatus();
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  switch (ts.reason) {
  case TR_ENABLED:
    if (ts.power > 100) {
      c = TFT_CYAN;
      snprintf(buf, sizeof(buf), "ACS %.1fkW", ts.power / 1000.0);
    } else {
      c = TFT_GREEN;
      snprintf(buf, sizeof(buf), "ACS OK");
    }
    break;
  case TR_PRICE:
    c = TFT_ORANGE;
    snprintf(buf, sizeof(buf), "ACS CARO");
    break;
  case TR_OVERLOAD:
    c = TFT_RED;
    snprintf(buf, sizeof(buf), "ACS CORTE");
    break;
  case TR_MANUAL:
    c = TFT_WHITE;
    snprintf(buf, sizeof(buf), "ACS OFF");
    break;
  default:
    c = TFT_DARKGREY;
    snprintf(buf, sizeof(buf), "ACS ?");
    break;
  }
  canvas.setTextColor(c);
  canvas.drawString(buf, 120, 17);

  // Price (Green < th, Orange < th+0.02, else Red)
  float price = getCurrentPrice();
  if (price < max_price_threshold) c = TFT_GREEN;
  else if (price < (max_price_threshold + 0.02)) c = TFT_ORANGE;
  else c = TFT_RED;
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(c);
  snprintf(buf, sizeof(buf), "%.3f EUR", price);
  canvas.drawString(buf, 120, 38);

  // Grid: exporting green, <5kW orange, else red
  if (current_grid_power < 0) c = TFT_GREEN;
  else if (current_grid_power < 5000) c = TFT_ORANGE;
  else c = TFT_RED;
  canvas.setTextColor(c);
  float grid_kw = (float)current_grid_power / 1000.0;
  snprintf(buf, sizeof(buf), "Red %s%.2f/%.1f", (grid_kw > 0 ? "+" : ""), grid_kw,
           (float)max_grid_power / 1000.0);
  canvas.drawString(buf, 120, 60);

  // Solar
  canvas.setTextColor(current_pv_power > 50 ? TFT_GREEN : TFT_ORANGE);
  snprintf(buf, sizeof(buf), "Solar %.2f kW", (float)current_pv_power / 1000.0);
  canvas.drawString(buf, 120, 86);

  // Beny power, big in the centre: Standby/0W green, <=2kW orange, >2kW red
  BenyData bd = getBenyData();
  if (bd.power < 100) c = TFT_GREEN;
  else if (bd.power <= 2000) c = TFT_ORANGE;
  else c = TFT_RED;
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(c);
  snprintf(buf, sizeof(buf), "%.2f kW", bd.power / 1000.0);
  canvas.drawString(buf, 120, 122);

  // Pool pump, left of the big number: today's run hours. Sky blue running on
  // solar, violet topping up at night, green max reached, grey waiting,
  // white manual. Hidden without the relay.
  PiscinaStatus ps = getPiscinaStatus();
  if (ps.reason != PR_OFFLINE) {
    switch (ps.reason) {
    case PR_SOLAR: c = TFT_SKYBLUE; break;
    case PR_FILL: c = TFT_VIOLET; break;
    case PR_DONE: c = TFT_GREEN; break;
    case PR_WAITING: c = TFT_DARKGREY; break;
    default: c = TFT_WHITE; break;
    }
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(c);
    snprintf(buf, sizeof(buf), "%.1fh", ps.hoursToday);
    canvas.drawString(buf, 24, 122);
  }

  // Mode
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  if (charging_mode == 0) {
    canvas.setTextColor(TFT_GREEN);
    canvas.drawString("SOLAR", 120, 156);
  } else {
    canvas.setTextColor(TFT_ORANGE);
    canvas.drawString("BALANCEO", 120, 156);
  }

  // Charge current: target vs. what the charger actually reports.
  // At BENY_MIN_AMPS the DLB has nothing left to give back, so it is flagged.
  canvas.setTextColor(target_amps <= BENY_MIN_AMPS ? TFT_YELLOW : TFT_GREEN);
  snprintf(buf, sizeof(buf), "%dA (%.0fA)", target_amps, bd.current);
  canvas.drawString(buf, 120, 180);

  // Status (short row near the bottom edge: truncate long states)
  canvas.setTextColor(TFT_WHITE);
  snprintf(buf, sizeof(buf), "%.12s", bd.status.c_str());
  canvas.drawString(buf, 120, 204);

  // WiFi down: red ring around the bezel, so an outage is obvious instead of
  // looking like a frozen device showing stale numbers
  if (WiFi.status() != WL_CONNECTED) {
    canvas.fillArc(120, 120, 119, 113, 0, 360, TFT_RED);
  }

  canvas.pushSprite(0, 0);
}

void wakeScreen() {
  lastInteractionTime = millis();
  if (!screenAwake) {
    M5Dial.Display.wakeup();
    M5Dial.Display.setBrightness(128);
    screenAwake = true;
    redraw = true;
    Serial.println("Screen: Wake up");
  }
}

void sleepScreen() {
  if (screenAwake) {
    M5Dial.Display.setBrightness(0);
    M5Dial.Display.sleep();
    screenAwake = false;
    Serial.println("Screen: Sleep");
  }
}

void loop() {
  // Reset WDT every loop
  esp_task_wdt_reset();

  // Read inputs FIRST so wake-up is immediate
  M5Dial.update();

  // Encoder turn or screen touch: wake-up only (for now; later: Tuya menu)
  long encoderPos = M5Dial.Encoder.read();
  if (encoderPos != lastEncoderPos) {
    lastEncoderPos = encoderPos;
    wakeScreen();
  }
  if (M5Dial.Touch.getCount() > 0) {
    wakeScreen();
  }

  // Button (pressing the dial, GPIO42): wake-up + mode change (only if awake)
  if (M5Dial.BtnA.wasPressed()) {
    bool wasAwake = screenAwake;
    wakeScreen(); // Always wake

    if (wasAwake) { // Only change mode if screen was already on
      charging_mode = (charging_mode + 1) % 2; // Now 2 modes (0=Solar, 1=Balanceo)
      saveMode(charging_mode);
      manual_logic_trigger = true;
      logEventf("MODO", "Dial: modo %s", charging_mode == 0 ? "SOLAR" : "BALANCEO");

      String modeStr = (charging_mode == 0) ? "SOLAR" : "BALANCEO";
      sendTelegramNotification("🔘 M5Dial Botón: Modo cambiado a " + modeStr);
    }
  }

  // Screen Wake-up from remote Telegram mode changes
  static int last_known_mode = -1;
  if (charging_mode != last_known_mode) {
    last_known_mode = charging_mode;
    wakeScreen();
  }

  // Screen timeout: sleep after inactivity
  if (millis() - lastInteractionTime > SCREEN_TIMEOUT) {
    sleepScreen();
  }

  // --- WIFI RECONNECT (escalating, non-blocking) ---
  // WiFi.setAutoReconnect() alone does not always recover an ESP32 STA when the
  // AP disappears for a while, so we escalate instead of just waiting: nudge
  // the existing config first, then tear the stack down and re-associate, and
  // only reboot as a genuine last resort. The reboot threshold is deliberately
  // long — a restart cannot fix an AP that is still down, it only throws away
  // uptime and hides the problem behind a screen full of dots.
  static unsigned long wifiDownSince = 0;
  static unsigned long lastWifiRetry = 0;
  static int wifiRetries = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSince == 0) {
      wifiDownSince = millis();
      lastWifiRetry = millis();
      wifiRetries = 0;
      Serial.println("WiFi: Desconectado, esperando reconexion...");
    } else if (millis() - lastWifiRetry > 15000) {
      lastWifiRetry = millis();
      wifiRetries++;

      if (wifiRetries % 4 != 0) {
        Serial.printf("WiFi: Reintento %d (reconnect)...\n", wifiRetries);
        WiFi.reconnect();
      } else {
        // Every 4th attempt (~1 min), rebuild the association from scratch
        Serial.printf("WiFi: Reintento %d (begin completo)...\n", wifiRetries);
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    }

    if (millis() - wifiDownSince > 900000) { // 15 min
      Serial.println("WiFi: Sin conexion 15min - reiniciando como ultimo recurso...");
      ESP.restart();
    }
  } else {
    if (wifiDownSince != 0) {
      logEventf("WIFI", "Reconectado tras %lus (%d reintentos)", (millis() - wifiDownSince) / 1000,
                wifiRetries);
      wifiDownSince = 0;
      wifiRetries = 0;
    }
  }

  // Update Tasks.
  // Every one of these needs the network. Running them without a link only
  // burns loop time on connections that cannot succeed (and TLS handshakes can
  // stall for seconds), which starves the reconnect logic above and the button
  // handling below. Skipping them keeps the device responsive during an outage.
  bool wifiUp = (WiFi.status() == WL_CONNECTED);

  if (wifiUp) {
    loopTelegram();
    loopGoogleSheets();

    static unsigned long lastMainLog = 0;
    if (millis() - lastMainLog > 10000) {
      lastMainLog = millis();
      Serial.println("MAIN: Calling loopHuawei...");
    }
    loopHuawei();

    loopBeny();
    loopEsios();
    loopTermo();
    loopPiscina();
  }

  // --- SCREEN DISPATCHER (0.5s) ---
  static unsigned long lastScreenUpdate = 0;
  if (millis() - lastScreenUpdate > 500) {
    lastScreenUpdate = millis();
    redraw = true;
  }

  // --- DLB LOGIC DISPATCHER (1s) ---
  // Only with a link: without it the Beny/Huawei readings are stale and no
  // command would reach the charger anyway.
  if (wifiUp && (millis() - lastLogicRun > logicInterval || manual_logic_trigger)) {
    lastLogicRun = millis();
    manual_logic_trigger = false;
    runSmartChargingLogic();
    runTermoLogic();
    runPiscinaLogic();
    redraw = true;
  }

  // --- LCD REDRAW ---
  if (redraw) {
    drawStatusScreen(false);
    redraw = false;
  }

  // --- TELEMETRY LOGGING (1s) ---
  static unsigned long lastTelemetry = 0;
  if (millis() - lastTelemetry > 1000) {
    lastTelemetry = millis();
    BenyData bdt = getBenyData();
    String modeName = (charging_mode == 0) ? "SOLAR" : "BALANC";
    // Format: [1s-LOG] Grid,Solar,BenyP,Status,Mode,TargetA
    Serial.printf("[1s-LOG] %d, %d, %.0f, %s, %s, %dA\n",
                  current_grid_power, current_pv_power, bdt.power,
                  bdt.status.c_str(), modeName.c_str(), target_amps);
  }

  delay(10); // Yield
}
