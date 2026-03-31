#include "BenyTask.h"
#include "EsiosTask.h"
#include "GoogleSheetsTask.h"
#include "HuaweiTask.h"
#include "TelegramTask.h"

// #include "WeatherTask.h" REMOVED

#include "config.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>

// WDT Timeout (seconds)
#define WDT_TIMEOUT 30

// Logic Constants
const float CHARGER_POWER_KW = 2.3;
// const int RELAY_PIN = 32; -> REMOVED

// Global State
// bool relay_state = false; -> REMOVED
unsigned long lastLogicRun = 0;
const unsigned long logicInterval = 2000; // Check every 2 seconds (DLB)

// ISR for Button A
volatile bool buttonPressed = false;
void IRAM_ATTR isrButtonA() {
  buttonPressed = true;
} // Kept for potential future use or debouncing

#include <Preferences.h>

Preferences preferences;

// Charging Algorithm Globals - Simplified
// Modes: 0=SOLAR, 1=BALANCEO, 2=TURBO
int charging_mode = 0;                       // Default: 0 (SOLAR)
int max_grid_power = DEFAULT_MAX_GRID_POWER; // Default from config
float max_price_threshold = PRICE_THRESHOLD; // Info only
bool manual_logic_trigger = false;           // Trigger for immediate logic run
int target_amps = BENY_MIN_AMPS;             // Start conservatively

// Screen Dimming State
unsigned long lastInteractionTime = 0;
const unsigned long SCREEN_TIMEOUT = 120000; // 2 minutes
bool screenAwake = true;

// Moving Average Filter State
int32_t grid_history[5] = {0};
int grid_history_index = 0;
bool grid_history_filled = false;

// Configurable Auto-Pause Globals
unsigned long pause_time_ms = 60000;
unsigned long resume_time_ms = 60000;
int resume_margin_watts = 1840;

bool auto_paused = false;
unsigned long time_exceeded = 0;
unsigned long time_available = 0;

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

void saveConfigVals() {
  preferences.begin("beny", false);
  preferences.putULong("t_pause", pause_time_ms);
  preferences.putULong("t_resume", resume_time_ms);
  preferences.putInt("r_margin", resume_margin_watts);
  preferences.end();
  Serial.println("Config Vals Saved.");
}

void setupOTA() {
  ArduinoOTA.setHostname("BenyChargerV2");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("OTA Ready");
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
  pause_time_ms = preferences.getULong("t_pause", 60000);
  resume_time_ms = preferences.getULong("t_resume", 60000);
  resume_margin_watts = preferences.getInt("r_margin", 1840);
  preferences.end();
  M5.Lcd.setRotation(3);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(2); // Revert to 2 (Size 3 too big)
  M5.Lcd.println("Init Dual DLB...");

  // Attach Interrupt for Button A (GPIO 37 on M5StickC Plus)
  pinMode(37, INPUT_PULLUP); // Ensure pullup
  attachInterrupt(digitalPinToInterrupt(37), isrButtonA, FALLING);

  Serial.begin(115200);
  // pinMode(RELAY_PIN, OUTPUT);
  // digitalWrite(RELAY_PIN, LOW);

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  M5.Lcd.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    M5.Lcd.print(".");
  }
  M5.Lcd.println(" OK");

  setupOTA(); // Init OTA

  // Configure Time
  configTime(3600, 3600, "pool.ntp.org");
  M5.Lcd.print("Time");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    M5.Lcd.print(".");
    delay(500);
  }
  M5.Lcd.println(" OK");

  // Init Tasks
  setupTelegram();
  setupGoogleSheets();

  setupBeny();

  setupHuawei();
  // setupWeather(); // Init weather (fetch forecast) REMOVED

  setupEsios(); // Fetches price immediately

  // Force Logic run immediately
  lastLogicRun = millis() - logicInterval;

  extern void sendTelegramNotification(String msg);
  String modeStr = (charging_mode == 0) ? "SOLAR" : (charging_mode == 1) ? "BALANCEO" : "TURBO";
  sendTelegramNotification("🚀 Sistema Iniciado. Modo actual: " + modeStr);
}

// planCharging function removed as demand_kwh is deprecated

// Logic to decide if we should charge NOW
// Logic to decide if we should charge NOW - REMOVED (Manual Control)
// bool shouldCharge() { ... }

void runSmartChargingLogic() {
  // PURE Dynamic Load Balancing (DLB) Logic
  // Goal: Maintain Grid Power <= MAX_GRID_POWER (6400W)
  // Action: Adjust Amps (6A - 22A)
  // Constraint: NEVER Start or Stop charging automatically.

  BenyData bd = getBenyData();

  // 1. CHECK STATUS: Only run logic if currently CHARGING (or if we are handling an auto-pause constraint)
  if (bd.status != "CHARGING" && !auto_paused) {
    // If NOT charging and NOT auto-paused, user explicitly stopped.
    return;
  }

  // --- AUTO PAUSE / RESTART LOGIC ---
  if (charging_mode != 2) { // Not Turbo
    if (!auto_paused) {
      bool exceed = false;
      if (charging_mode == 0) { // Solar
        if (current_grid_power > 690) exceed = true;
      } else if (charging_mode == 1) { // Balanceo
        if (current_grid_power > max_grid_power) exceed = true;
      }
      
      if (exceed) {
        if (time_exceeded == 0) time_exceeded = millis();
        if (millis() - time_exceeded >= pause_time_ms) {
          auto_paused = true;
          benyStopCharge();
          Serial.println("Autoapagado: Pausando por exceso de red.");
          time_exceeded = 0;
          return; // Skip DLB this cycle
        }
      } else {
        time_exceeded = 0;
      }
    } else {
      // Is paused
      float current_limit = (charging_mode == 0) ? 0 : max_grid_power;
      bool available = (current_limit - current_grid_power) >= resume_margin_watts;
      
      if (available) {
        if (time_available == 0) time_available = millis();
        if (millis() - time_available >= resume_time_ms) {
          auto_paused = false;
          benyStartCharge();
          Serial.println("Autoreinicio: Reanudando carga.");
          time_available = 0;
        }
      } else {
        time_available = 0;
      }
      return; // Skip DLB while paused
    }
  } else {
    // In Turbo mode, if previously auto-paused, force restart
    if (auto_paused) {
      auto_paused = false;
      benyStartCharge();
      Serial.println("Turbo activado: Forzando reanudacion.");
    }
  }

  // --- MOVING AVERAGE FILTER FOR GRID POWER ---
  grid_history[grid_history_index] = current_grid_power;
  grid_history_index++;
  if (grid_history_index >= 5) {
    grid_history_index = 0;
    grid_history_filled = true;
  }
  
  int count = grid_history_filled ? 5 : (grid_history_index == 0 ? 1 : grid_history_index);
  int32_t sum = 0;
  for (int i = 0; i < count; i++) {
    sum += grid_history[i];
  }
  int32_t avg_grid_power = sum / count;

  // 2. READ GRID POWER (Instant used above for auto-pause, avg used here for DLB)
  // We want to keep Import <= LIMIT.

  // 3. CALCULATE AVAILABLE HEADROOM
  // Headroom = Limit - Current
  float limit_watts = max_grid_power; // Default Mode 1 or 2 (Balanceo/Turbo)

  if (charging_mode == 0) {
    // Mode 0: Solar / Surplus
    // Target: 0 Grid Import
    limit_watts = 0;
  }

  float error_watts = limit_watts - avg_grid_power;

  // 4. CALCULATE IDEAL AMPS
  // Change needed = Error / 230V
  int amps_delta = (int)(error_watts / 230.0);

  int current_set_amps = target_amps; // Our last known setpoint
  int ideal_amps = current_set_amps + amps_delta;

  Serial.printf("DLB: Grid %d, Limit %.0f, Err %.0f, CurSet %d -> Ideal %d\n",
                current_grid_power, limit_watts, error_watts, current_set_amps,
                ideal_amps);

  // 5. CLAMPING
  // Min: 6A (1.4kW) - Cannot go lower without stopping (forbidden).
  if (ideal_amps < BENY_MIN_AMPS) {
    Serial.println("DLB: Ideal < 6A. Clamping to 6A (Cannot Stop).");
    ideal_amps = BENY_MIN_AMPS;
  }
  // Max: 22A
  if (ideal_amps > BENY_MAX_AMPS) {
    ideal_amps = BENY_MAX_AMPS;
  }

  // 6. ACTUATION
  // Check if we need to update:
  // 1. If calculated different from target var (Change needed)
  // 2. If reported amps different from target var (Sync needed)
  bool sync_needed = (abs(bd.current - target_amps) > 2);

  if (ideal_amps != target_amps || sync_needed) {
    Serial.printf("DLB: Adjusting %d -> %dA (Phys: %.1fA)\n", target_amps,
                  ideal_amps, bd.current);
    target_amps = ideal_amps;
    benySetCurrent(target_amps);
  } else {
    // Optional: periodic refresh every X loops?
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
  M5.Lcd.printf("Grid: %.3f/%.3f\n", (float)current_grid_power / 1000.0,
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
  if (charging_mode == 2) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.printf("Mode: TURBO   \n");
  } else if (charging_mode == 1) {
    M5.Lcd.setTextColor(ORANGE, BLACK);
    M5.Lcd.printf("Mode: BALANCEO\n");
  } else {
    M5.Lcd.setTextColor(GREEN, BLACK);
    M5.Lcd.printf("Mode: SOLAR   \n");
  }

  // Auto-pause warning
  if (auto_paused) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.printf(" PAUSA AUTO (Exceso)   \n");
  }

  // Line 3: Status (Pad with spaces to overwrite previous long text)
  // "Stat: 1234567890123456" (Max ~20 chars)
  char statBuf[30];
  snprintf(statBuf, sizeof(statBuf), "Stat: %s                ",
           bd.status.c_str());
  M5.Lcd.printf("%.20s\n", statBuf); // Limit to screen width

  // Reset to White
  M5.Lcd.setTextColor(WHITE, BLACK);
}

// void drawForecastScreen(bool fullClear) ... REMOVED

// ... (in setup)
// pinMode(37, INPUT); // Handled by M5.begin() but verified
// attachInterrupt(37, isrButtonA, FALLING); // BtnA is usually active low? Need
// to check. M5StickC uses GPIO37. Actually M5StickC Plus documentation says
// BtnA is GPIO 37.

void wakeScreen() {
  lastInteractionTime = millis();
  if (!screenAwake) {
    M5.Lcd.writecommand(0x29); // ST7789 DISPON - Display ON
    screenAwake = true;
    redraw = true;
    Serial.println("Screen: Wake up");
  }
}

void sleepScreen() {
  if (screenAwake) {
    M5.Lcd.writecommand(0x28); // ST7789 DISPOFF - Display OFF (backlight stays but shows black)
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
      charging_mode = (charging_mode + 1) % 3;
      saveMode(charging_mode);
      manual_logic_trigger = true;
      Serial.printf("Button A Pressed: Mode set to %d\n", charging_mode);
      
      extern void sendTelegramNotification(String msg);
      String modeStr = (charging_mode == 0) ? "SOLAR" : (charging_mode == 1) ? "BALANCEO" : "TURBO";
      sendTelegramNotification("🔘 M5Stick Botón: Modo cambiado a " + modeStr);
    }
  }

  // Screen Wake-up from remote Telegram changes or auto-pause alerts
  static int last_known_mode = -1;
  if (charging_mode != last_known_mode) {
    last_known_mode = charging_mode;
    wakeScreen();
  }

  static bool last_auto_paused = false;
  if (auto_paused != last_auto_paused) {
    last_auto_paused = auto_paused;
    wakeScreen();
  }
  
  // Screen timeout: sleep after inactivity
  if (millis() - lastInteractionTime > SCREEN_TIMEOUT) {
    sleepScreen();
  }

  // Handle OTA
  ArduinoOTA.handle();

  // Update Tasks
  loopTelegram();
  loopGoogleSheets();
  loopHuawei();
  loopBeny();
  loopEsios();

  // loopWeather(); // Updates forecast hourly REMOVED

  // Run Logic Periodically
  // Screen Refresh (0.5s)
  static unsigned long lastScreenUpdate = 0;
  if (millis() - lastScreenUpdate > 500) {
    lastScreenUpdate = millis();
    redraw = true;
  }
  if (millis() - lastLogicRun > logicInterval || manual_logic_trigger) {
    lastLogicRun = millis();
    manual_logic_trigger = false; // Reset trigger

    runSmartChargingLogic();
    // bool intent = shouldCharge(); REMOVED, inside SmartLogic

    // Relay Actuation REMOVED
    // if (intent != relay_state) { ... }

    // Decrement required_kwh if charging
    // Decrement required_kwh simulation REMOVED (Relay not active)

    // Logic run forces a redraw to update values
    redraw = true;
  }

  // Draw Screen if needed
  if (redraw) {
    drawStatusScreen(false); // Always Status. Optimized.
    redraw = false;
  }

  // Minimal delay to prevent WDT and allow background tasks
  delay(10);
}
