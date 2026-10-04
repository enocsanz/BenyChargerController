#include "BenyTask.h"
#include "EsiosTask.h"
#include "GoogleSheetsTask.h"
#include "HuaweiTask.h"
#include "TelegramTask.h"
#include "TermoTask.h"
#include "PiscinaTask.h"
#include "SondaLink.h"
#include "EnvSensor.h"
#include "WifiRoam.h"

#include "config.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
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

// --- M5StampS3 I/O ---
// No screen: Telegram shows everything. The board's button toggles the
// charging mode and its RGB LED gives the state at a glance.
const int BUTTON_PIN = 0;      // G0 (also BOOT: held at power-up = download mode)
const int LED_PIN = 21;        // WS2812 on G21
const uint8_t LED_LEVEL = 24;  // of 255: the LED is very bright at full

// Firmware identity, shown on boot and by /version: after an OTA update it
// confirms the new build is the one running
const char *FW_BUILD = __DATE__ " " __TIME__;

#include <Preferences.h>

Preferences preferences;

// Charging Algorithm Globals - Simplified
// Modes: 0=SOLAR, 1=BALANCEO
int charging_mode = 0;                       // Default: 0 (SOLAR)
int max_grid_power = DEFAULT_MAX_GRID_POWER; // Default from config
float max_price_threshold = PRICE_THRESHOLD; // Info only
bool manual_logic_trigger = false;           // Trigger for immediate logic run
int target_amps = BENY_MIN_AMPS;             // Start conservatively

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

// Status LED, by priority:
//   red    no WiFi (nothing can be read or commanded)
//   orange overload: heater cut by overload, or car at its floor over the limit
//   blue   car charging
//   green  all fine
// A dim white flash every 2s on top of the colour shows the loop is alive.
void updateLed() {
  static unsigned long lastLed = 0;
  if (millis() - lastLed < 200) return;
  lastLed = millis();

  uint8_t r = 0, g = 0, b = 0;
  BenyData bd = getBenyData();
  bool charging = (bd.status == "CHARGING" || bd.status == "STARTING");
  bool overload = getTermoStatus().reason == TR_OVERLOAD ||
                  (charging && target_amps <= BENY_MIN_AMPS &&
                   current_grid_power > CONTRACTED_POWER + GRID_DEADBAND);

  if (WiFi.status() != WL_CONNECTED) r = LED_LEVEL;
  else if (overload) { r = LED_LEVEL; g = LED_LEVEL / 3; }
  else if (charging) b = LED_LEVEL;
  else g = LED_LEVEL;

  if (millis() % 2000 < 200) r = g = b = LED_LEVEL / 4; // heartbeat
  neopixelWrite(LED_PIN, r, g, b);
}

// --- OTA (firmware update over WiFi) ---
// pio run -e stamps3_ota -t upload. ArduinoOTA receives the whole image inside
// handle(), so the control loop pauses for the ~1 min it takes (the relays
// keep their failsafe countdown). The new image goes to the other OTA slot:
// if the transfer fails, the current firmware keeps running.
bool otaReady = false;

void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("OTA: Recibiendo firmware..."); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    esp_task_wdt_reset(); // the transfer runs inside handle(), past the 30s
    bool on = (millis() / 250) % 2;
    neopixelWrite(LED_PIN, on ? LED_LEVEL : 0, 0, on ? LED_LEVEL : 0); // purple blink
    static int lastTen = -1;
    int pct = total ? done * 100 / total : 0;
    if (pct / 10 != lastTen) {
      lastTen = pct / 10;
      Serial.printf("OTA: %d%%\n", pct);
    }
  });
  ArduinoOTA.onEnd([]() { Serial.println("OTA: Completada, reiniciando"); });
  ArduinoOTA.onError([](ota_error_t e) { logEventf("OTA", "Error %u: sigue el firmware anterior", e); });
  ArduinoOTA.begin();
  otaReady = true;
  Serial.printf("OTA: Lista en %s (%s)\n", WiFi.localIP().toString().c_str(), OTA_HOSTNAME);
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  neopixelWrite(LED_PIN, 0, 0, LED_LEVEL); // blue while booting

  // WDT Init
  esp_task_wdt_init(WDT_TIMEOUT, true); // Enable panic (reset) on timeout
  esp_task_wdt_add(NULL);               // Add current thread to WDT

  // recover mode and configs
  preferences.begin("beny", true);               // Read only
  charging_mode = preferences.getInt("mode", 0); // Default 0
  max_grid_power = preferences.getInt("limit", DEFAULT_MAX_GRID_POWER);
  preferences.end();

  Serial.begin(115200);
  Serial.println("Beny DLB - M5StampS3");

  // WiFi. Not persistent: the IDF would otherwise keep the last access point in
  // flash and reuse it, instead of choosing the strongest one at each boot.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Modem sleep adds latency to the Beny UDP / Modbus polls
  WiFi.setAutoReconnect(true);
  // Mesh network (several Google Wifi points, same SSID): by default the
  // ESP32 joins the FIRST point its scan finds, not the closest one. In the
  // pool house, with a point right there, it got -79/-82 dBm from a far one.
  // Scan every channel and join the strongest. Kept for every later begin().
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    esp_task_wdt_reset(); // Prevent WDT reset while connecting
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
    setupOta();
    setupSondaLink();
  } else {
    // Do NOT reboot here. Rebooting on a failed boot-time connect turned a
    // router outage into an endless boot loop. Boot anyway and let loop()
    // own reconnection.
    Serial.println("WiFi: Sin conexion al arrancar. Continuando, loop() reintentara.");
  }

  // Configure Time — proper DST handling for Spain (CET/CEST).
  // Safe to call without a link: the SNTP client syncs on its own once the
  // network comes up, so a boot without WiFi still gets the time later.
  // Several servers: the M5Dial also had a battery RTC to fall back on, the
  // StampS3 has none, and a single server once took ~2 min to answer.
  configTime(0, 0, "es.pool.ntp.org", "time.google.com", "pool.ntp.org");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  if (WiFi.status() == WL_CONNECTED) {
    struct tm timeinfo;
    unsigned long ntpStart = millis();
    bool timeSet = false;
    while (!(timeSet = getLocalTime(&timeinfo, 1000))) {
      if (millis() - ntpStart > 10000) {
        Serial.println("\nNTP: Timeout, continuing without correct time.");
        break;
      }
      esp_task_wdt_reset(); // Prevent WDT reset during NTP sync
    }
    if (timeSet) Serial.println("Hora OK");
  }

  // Init Tasks
  setupTelegram();
  setupGoogleSheets();

  setupBeny();

  setupHuawei();

  setupEsios(); // Fetches price immediately

  setupTermo();
  setupPiscina();
  setupEnvSensor();

  // Force Logic run immediately
  lastLogicRun = millis() - logicInterval;

  String modeStr = (charging_mode == 0) ? "SOLAR" : "BALANCEO";

  // Why we booted: a watchdog or panic reset here is worth knowing about
  static const char *resetNames[] = {"desconocido", "encendido", "externo", "software",
                                     "panic",       "int_wdt",   "task_wdt", "wdt",
                                     "deepsleep",   "brownout",  "sdio"};
  int rr = (int)esp_reset_reason();
  String reason = (rr >= 0 && rr <= 10) ? resetNames[rr] : "?";
  sendTelegramNotification("🚀 Sistema Iniciado (" + reason + "). Modo actual: " + modeStr +
                           "\nFirmware " + String(FW_BUILD));
  logEvent("ARRANQUE", "Reinicio: " + reason + ", modo " + modeStr + ", CPU " +
                           String(getCpuFrequencyMhz()) + " MHz, firmware " + String(FW_BUILD));
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

// Button: toggles Solar <-> Balanceo on release, with a 50ms debounce
void checkButton() {
  static bool lastStable = HIGH, lastRead = HIGH;
  static unsigned long changedAt = 0;
  bool now = digitalRead(BUTTON_PIN);
  if (now != lastRead) {
    lastRead = now;
    changedAt = millis();
  }
  if (millis() - changedAt > 50 && now != lastStable) {
    lastStable = now;
    if (now == HIGH) { // released
      charging_mode = (charging_mode + 1) % 2;
      saveMode(charging_mode);
      manual_logic_trigger = true;
      String modeStr = (charging_mode == 0) ? "SOLAR" : "BALANCEO";
      logEvent("MODO", "Boton: modo " + modeStr);
      sendTelegramNotification("🔘 Botón: Modo cambiado a " + modeStr);
    }
  }
}

void loop() {
  // Reset WDT every loop
  esp_task_wdt_reset();

  checkButton();

  // --- WIFI RECONNECT (escalating, non-blocking) ---
  // WiFi.setAutoReconnect() alone does not always recover an ESP32 STA when the
  // AP disappears for a while, so we escalate instead of just waiting: nudge
  // the existing config first, then tear the stack down and re-associate, and
  // only reboot as a genuine last resort. The reboot threshold is deliberately
  // long — a restart cannot fix an AP that is still down, it only throws away
  // uptime and hides the problem.
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
  // handling. Skipping them keeps the device responsive during an outage.
  bool wifiUp = (WiFi.status() == WL_CONNECTED);

  // The watchdog is fed between tasks: each one stays well under the 30s
  // timeout on its own, but several slow network calls in the same pass
  // (Telegram, the Sheets batch, ESIOS, reconnecting a relay) added up past
  // it once (task_wdt reset on 01/10 at 22:08).
  if (wifiUp) {
    if (!otaReady) { // WiFi was down at boot
      setupOta();
      setupSondaLink();
    }
    ArduinoOTA.handle();
    esp_task_wdt_reset();

    loopTelegram();
    esp_task_wdt_reset();
    loopGoogleSheets();
    esp_task_wdt_reset();

    static unsigned long lastMainLog = 0;
    if (millis() - lastMainLog > 10000) {
      lastMainLog = millis();
      Serial.println("MAIN: Calling loopHuawei...");
    }
    loopHuawei();

    loopBeny();
    loopEsios();
    esp_task_wdt_reset();
    loopTermo();
    esp_task_wdt_reset();
    loopPiscina();
    esp_task_wdt_reset();
  }

  // --- DLB LOGIC DISPATCHER (1s) ---
  // Only with a link: without it the Beny/Huawei readings are stale and no
  // command would reach the charger anyway.
  if (wifiUp && (millis() - lastLogicRun > logicInterval || manual_logic_trigger)) {
    lastLogicRun = millis();
    manual_logic_trigger = false;
    // Negative surplus price periods: only recorded (no action, no alert)
    static bool wasNegative = false;
    bool negative = surplusPriceNegative();
    if (negative != wasNegative) {
      wasNegative = negative;
      float sp = getCurrentSurplusPrice();
      if (negative) logEventf("EXCEDENTES", "Precio negativo: %.4f E/kWh", sp);
      else logEventf("EXCEDENTES", "Precio vuelve a positivo: %.4f E/kWh", sp);
    }
    runSmartChargingLogic();
    runTermoLogic();
    runPiscinaLogic();
  }

  loopSondaLink();
  loopEnvSensor();
  if (wifiUp) wifiRoamLoop(WIFI_SSID, WIFI_PASSWORD, [](const String &m) { logEvent("WIFI", m); });
  updateLed();

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
