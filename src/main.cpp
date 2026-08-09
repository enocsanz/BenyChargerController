#include "BenyTask.h"
#include "EsiosTask.h"
#include "GoogleSheetsTask.h"
#include "HuaweiTask.h"
#include "TelegramTask.h"

#include "config.h"
#include <Arduino.h>
#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>

// WDT Timeout (seconds)
#define WDT_TIMEOUT 30

// Logic Constants
// Dead band around the DLB target, to avoid amp jitter (W)
const int32_t GRID_DEADBAND = 200;

// Queues a message for the Telegram task. Never blocks: the actual HTTPS
// request is issued from loopTelegram().
extern void sendTelegramNotification(String msg);

// Global State
unsigned long lastLogicRun = 0;
const unsigned long logicInterval = 1000; // Check every 1 second (Faster DLB)

// ISR for Button A
volatile bool buttonPressed = false;
void IRAM_ATTR isrButtonA() {
  buttonPressed = true;
} // Kept for potential future use or debouncing

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
  M5.begin();

  // WDT Init
  esp_task_wdt_init(WDT_TIMEOUT, true); // Enable panic (reset) on timeout
  esp_task_wdt_add(NULL);               // Add current thread to WDT

  // recover mode and configs
  preferences.begin("beny", true);               // Read only
  charging_mode = preferences.getInt("mode", 0); // Default 0
  max_grid_power = preferences.getInt("limit", DEFAULT_MAX_GRID_POWER);
  preferences.end();
  M5.Lcd.setRotation(3);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(2); // Revert to 2 (Size 3 too big)
  M5.Lcd.println("Init Dual DLB...");

  // Attach Interrupt for Button A (GPIO 37 on M5StickC Plus)
  pinMode(37, INPUT_PULLUP); // Ensure pullup
  attachInterrupt(digitalPinToInterrupt(37), isrButtonA, FALLING);

  Serial.begin(115200);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Modem sleep adds latency to the Beny UDP / Modbus polls
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  M5.Lcd.print("WiFi");
  unsigned long wifiStart = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    if (++dots <= 16) M5.Lcd.print("."); // Bounded: never push the screen around
    esp_task_wdt_reset();                // Prevent WDT reset while connecting
  }

  if (WiFi.status() == WL_CONNECTED) {
    M5.Lcd.println(" OK");
  } else {
    // Do NOT reboot here. Rebooting on a failed boot-time connect turned a
    // router outage into an endless boot loop (20s of dots -> restart -> 20s
    // of dots...), which is what the "black screen filling with dots" is.
    // Boot anyway and let loop() own reconnection.
    M5.Lcd.println(" SIN RED");
    Serial.println("WiFi: Sin conexion al arrancar. Continuando, loop() reintentara.");
  }

  // Configure Time — proper DST handling for Spain (CET/CEST).
  // Safe to call without a link: the SNTP client syncs on its own once the
  // network comes up, so a boot without WiFi still gets the time later.
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  if (WiFi.status() == WL_CONNECTED) {
    M5.Lcd.print("Time");
    struct tm timeinfo;
    unsigned long ntpStart = millis();
    bool timeSet = false;
    while (!(timeSet = getLocalTime(&timeinfo, 1000))) {
      if (millis() - ntpStart > 10000) {
        Serial.println("\nNTP: Timeout, continuing without correct time.");
        M5.Lcd.print(" FAIL");
        break;
      }
      M5.Lcd.print(".");
      esp_task_wdt_reset(); // Prevent WDT reset during NTP sync
    }
    if (timeSet) M5.Lcd.println(" OK");
  }

  // Init Tasks
  setupTelegram();
  setupGoogleSheets();

  setupBeny();

  setupHuawei();
  // setupWeather(); // Init weather (fetch forecast) REMOVED

  setupEsios(); // Fetches price immediately

  // Force Logic run immediately
  lastLogicRun = millis() - logicInterval;

  String modeStr = (charging_mode == 0) ? "SOLAR" : "BALANCEO";
  sendTelegramNotification("🚀 Sistema Iniciado. Modo actual: " + modeStr);
}

void runSmartChargingLogic() {
  // PURE Dynamic Load Balancing (DLB) Logic
  // Goal: Keep grid power at the mode's target by modulating the charge current
  // Action: Adjust Amps (BENY_MIN_AMPS - BENY_MAX_AMPS), 1A per second
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

  // --- STEP-BY-STEP DLB (±1A per second) ---
  int32_t limit_watts = (charging_mode == 0) ? SOLAR_GRID_TARGET : max_grid_power;
  int ideal_amps = target_amps;

  // Hysteresis: Skip adjustment if within GRID_DEADBAND of limit to avoid jitter
  if (current_grid_power > (limit_watts + GRID_DEADBAND)) {
    // Over the limit -> Reduce by 1A
    ideal_amps = target_amps - 1;
  } else if (current_grid_power < (limit_watts - GRID_DEADBAND)) {
    // Under the limit -> Increase by 1A
    ideal_amps = target_amps + 1;
  }

  // 4. CLAMPING
  if (ideal_amps < BENY_MIN_AMPS) ideal_amps = BENY_MIN_AMPS;
  if (ideal_amps > BENY_MAX_AMPS) ideal_amps = BENY_MAX_AMPS;

  // 5. ACTUATION & SYNC
  // Sync if reported physical current is significantly different from target
  bool sync_needed = (abs(bd.current - target_amps) > 2);

  if (ideal_amps != target_amps || sync_needed) {
    Serial.printf("DLB: Grid %d, Limit %d | Adjusting %d -> %dA (Phys: %.1fA)\n",
                  current_grid_power, limit_watts, target_amps, ideal_amps, bd.current);
    target_amps = ideal_amps;
    benySetCurrent(target_amps);
  }
}

// --- UI Logic ---
bool redraw = true;

void drawStatusScreen(bool fullClear) {
  if (fullClear) {
    M5.Lcd.fillScreen(BLACK);
  }
  M5.Lcd.setCursor(0, 0);

  // Price (Green < th, Orange < th+0.02, else Red)
  float price = getCurrentPrice();
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.print("P: ");

  if (price < max_price_threshold) {
    M5.Lcd.setTextColor(GREEN, BLACK);
  } else if (price < (max_price_threshold + 0.02)) {
    M5.Lcd.setTextColor(ORANGE, BLACK);
  } else {
    M5.Lcd.setTextColor(RED, BLACK);
  }
  M5.Lcd.printf("%.3f          \n", price);

  // Grid
  if (current_grid_power < 0) {
    M5.Lcd.setTextColor(GREEN, BLACK); // Exporting
  } else if (current_grid_power < 5000) {
    M5.Lcd.setTextColor(ORANGE, BLACK); // Importing < 5kW
  } else {
    M5.Lcd.setTextColor(RED, BLACK); // Importing > 5kW
  }
  float grid_kw = (float)current_grid_power / 1000.0;
  M5.Lcd.printf("Grid: %s%.3f/%.1f \n", (grid_kw > 0 ? "+" : ""), grid_kw,
                (float)max_grid_power / 1000.0);

  // Solar
  if (current_pv_power > 50) { // Producing > 50W
    M5.Lcd.setTextColor(GREEN, BLACK);
  } else {
    M5.Lcd.setTextColor(ORANGE, BLACK);
  }
  M5.Lcd.printf("Solar: %.3fkW     \n", (float)current_pv_power / 1000.0);

  // Beny Display (3 Lines)
  BenyData bd = getBenyData();

  // Color Logic: Standby/0W(Green), <=2kW(Orange), >2kW(Red)
  if (bd.power < 100) { // Approx 0W
    M5.Lcd.setTextColor(GREEN, BLACK);
  } else if (bd.power <= 2000) {
    M5.Lcd.setTextColor(ORANGE, BLACK);
  } else {
    M5.Lcd.setTextColor(RED, BLACK);
  }

  // Line 1: Header + Power
  M5.Lcd.printf("Beny: %.3fkW\n", bd.power / 1000.0);

  // Line 2: Mode
  if (charging_mode == 0) {
    M5.Lcd.setTextColor(GREEN, BLACK);
    M5.Lcd.printf("Mode: SOLAR   \n");
  } else if (charging_mode == 1) {
    M5.Lcd.setTextColor(ORANGE, BLACK);
    M5.Lcd.printf("Mode: BALANCEO\n");
  }

  // Charge current: target vs. what the charger actually reports.
  // At BENY_MIN_AMPS the DLB has nothing left to give back, so it is flagged.
  if (target_amps <= BENY_MIN_AMPS) {
    M5.Lcd.setTextColor(YELLOW, BLACK); // At the floor, cannot reduce further
  } else {
    M5.Lcd.setTextColor(GREEN, BLACK);
  }
  M5.Lcd.printf("Amp: %2dA (%2.0fA)   \n", target_amps, bd.current);

  // Line 3: Status (Pad with spaces to overwrite previous long text)
  // "Stat: 1234567890123456" (Max ~20 chars)
  char statBuf[30];
  snprintf(statBuf, sizeof(statBuf), "Stat: %s                ",
           bd.status.c_str());
  M5.Lcd.printf("%.20s\n", statBuf); // Limit to screen width

  // WiFi: only shown when down, so an outage is obvious instead of looking
  // like a frozen device showing stale numbers
  if (WiFi.status() != WL_CONNECTED) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.printf("SIN WIFI - reintent.\n");
  } else {
    M5.Lcd.printf("                    \n");
  }

  // Reset to White
  M5.Lcd.setTextColor(WHITE, BLACK);
}

void wakeScreen() {
  lastInteractionTime = millis();
  if (!screenAwake) {
    M5.Axp.SetLDO2(true);        // Power on LDO2 (Backlight)
    M5.Axp.ScreenBreath(100);    // Max brightness (M5StickC-Plus uses 0-100)
    M5.Lcd.writecommand(0x29);  // ST7789 DISPON
    screenAwake = true;
    redraw = true;
    Serial.println("Screen: Wake up");
  }
}

void sleepScreen() {
  if (screenAwake) {
    M5.Axp.ScreenBreath(0);      // Dims to zero
    M5.Axp.SetLDO2(false);       // Cuts power to backlight
    M5.Lcd.writecommand(0x28);  // ST7789 DISPOFF
    screenAwake = false;
    Serial.println("Screen: Sleep");
  }
}

void loop() {
  // Reset WDT every loop
  esp_task_wdt_reset();

  // Read buttons FIRST so wake-up is immediate
  M5.update();

  // Button B: dedicated wake-up only
  if (M5.BtnB.wasPressed()) {
    wakeScreen();
  }

  // Button A: wake-up + mode change (only if already awake)
  if (M5.BtnA.wasPressed() || buttonPressed) {
    buttonPressed = false;
    bool wasAwake = screenAwake;
    wakeScreen(); // Always wake

    if (wasAwake) { // Only change mode if screen was already on
      charging_mode = (charging_mode + 1) % 2; // Now 2 modes (0=Solar, 1=Balanceo)
      saveMode(charging_mode);
      manual_logic_trigger = true;
      Serial.printf("Button A Pressed: Mode set to %d\n", charging_mode);

      String modeStr = (charging_mode == 0) ? "SOLAR" : "BALANCEO";
      sendTelegramNotification("🔘 M5Stick Botón: Modo cambiado a " + modeStr);
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
      Serial.printf("WiFi: Reconectado tras %lus (%d reintentos). IP: %s\n",
                    (millis() - wifiDownSince) / 1000, wifiRetries,
                    WiFi.localIP().toString().c_str());
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
