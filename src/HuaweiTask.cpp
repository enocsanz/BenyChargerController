#include "HuaweiTask.h"
#include "config.h"
#include <Arduino.h>
#include <HTTPClient.h>

// --- Configuration & Constants ---
// Using emelianov/modbus-esp8266
ModbusIP mb;

// Data Globals
int32_t current_grid_power = 0;
int32_t current_pv_power = 0;
uint16_t gridPowerBuf[2]; // Buffer for 2x16-bit registers
int lastRequestType = 0;  // 0 = Grid, 1 = PV

// State Machine
enum HuaweiState { H_INIT, H_WIFI_WAIT, H_CONNECTING, H_READING, H_BACKOFF };

HuaweiState currentState = H_INIT;
unsigned long stateEntryTime = 0;

// Timers & Intervals
const unsigned long READ_INTERVAL_NORMAL =
    1000; // Read every 1s (faster updates)
const unsigned long READ_INTERVAL_SLOW = 10000; // Slow down if congested
unsigned long readInterval = READ_INTERVAL_NORMAL;
unsigned long lastReadTime = 0;
unsigned long lastStateChange = 0;

// Robustness
IPAddress inverterIp;
bool ipParsed = false;
int errorCount = 0;
const int MAX_ERRORS = 3;

// Metrics
unsigned long lastRequestTime = 0; // To measure latency

// Forward Declarations
void changeState(HuaweiState newState);
void restartEW11(); // Keep the EW11 restarter just in case

// --- Callbacks ---
bool cbReadPower(Modbus::ResultCode event, uint16_t transactionId, void *data) {
  unsigned long latency = millis() - lastRequestTime;

  if (event == Modbus::EX_SUCCESS) {
    // Saturation Check: If latency > 200ms, slow down
    if (latency > 200) {
      readInterval = READ_INTERVAL_SLOW;
      // Serial.printf("Huawei: Slow response (%lums). Throttling.\n", latency);
    } else {
      readInterval = READ_INTERVAL_NORMAL;
    }

    // Huawei sends Big Endian (Int32)
    int32_t raw = (int32_t)((gridPowerBuf[0] << 16) | gridPowerBuf[1]);

    // Determine what we just read based on context or user argument 'data'
    // But 'data' is passed from readHreg. Let's use a simple global flag or
    // just deduce functionality.
    // Simpler: use the transactionId or just a toggling logic in loop.
    // For now, let's look at the raw value to sanity check? No, raw can be
    // anything. Rely on the logic that issued the command.
    // WE WILL HANDLE DECODING IN THE MAIN LOOP OR ASSUME SEQUENTIAL EXECUTION.
    // Actually, the callback doesn't easily tell us WHICH register was read
    // unless we pass it in 'data'.
    // Let's pass (void*)0 for Grid, (void*)1 for PV.

    // 0 = Grid, 1 = PV
    if (lastRequestType == 0) {
      // Huawei Smart Meter:
      // Standard: Positive = Import, Negative = Export.
      // Some installs are wired backwards.
      if (HUAWEI_INVERT_POWER) {
        current_grid_power = -raw;
      } else {
        current_grid_power = raw;
      }

#ifdef DEBUG_HUAWEI
      Serial.printf("Huawei: Grid Raw %d -> %d W (Lat: %lums)\n", raw,
                    current_grid_power, latency);
#endif
    } else { // PV
      current_pv_power = raw;
#ifdef DEBUG_HUAWEI
      Serial.printf("Huawei: PV %d W\n", current_pv_power);
#endif
    }

    errorCount = 0; // Reset errors on success
  } else {
    Serial.printf("Huawei: Modbus Error 0x%02X (Lat: %lums)\n", event, latency);
    errorCount++;
  }
  return true;
}

// --- Setup ---
void setupHuawei() {
  mb.client(); // Init ModbusIP as client
  changeState(H_INIT);

  if (!ipParsed) {
    inverterIp.fromString(INVERTER_IP);
    ipParsed = true;
  }
}

// --- State Helpers ---
void changeState(HuaweiState newState) {
  currentState = newState;
  stateEntryTime = millis();
  lastStateChange = millis();

  String sName;
  switch (newState) {
  case H_INIT:
    sName = "INIT";
    break;
  case H_WIFI_WAIT:
    sName = "WIFI_WAIT";
    break;
  case H_CONNECTING:
    sName = "CONNECTING";
    break;
  case H_READING:
    sName = "READING";
    break;
  case H_BACKOFF:
    sName = "BACKOFF";
    break;
  }
  Serial.printf("Huawei: State -> %s\n", sName.c_str());
}

// --- Main Loop ---
void loopHuawei() {
  // Always run mb.task() if we think we are connected or trying to connect
  // But be careful not to run it if we want to be "quiet"
  if (currentState == H_READING || currentState == H_CONNECTING) {
    mb.task();
  }

  switch (currentState) {
  case H_INIT:
    // Wait 5s on boot to allow WiFi to stabilize and EW11 to settle
    if (millis() - stateEntryTime > 5000) {
      changeState(H_WIFI_WAIT);
    }
    break;

  case H_WIFI_WAIT:
    if (WiFi.status() == WL_CONNECTED) {
      changeState(H_CONNECTING);
    }
    break;

  case H_CONNECTING:
    if (WiFi.status() != WL_CONNECTED) {
      changeState(H_WIFI_WAIT);
      return;
    }

    // Attempt Connection
    // Force cleanup first just in case
    // mb.disconnect(inverterIp); // Ensure clean slate?
    // Actually emelianov lib doesn't strictly require disconnect before
    // connect, but good practice if we are retrying.

    Serial.printf("Huawei: Connecting to %s...\n", INVERTER_IP);
    mb.connect(inverterIp, INVERTER_PORT);

    // Give it a moment? internal task() handles the handshake.
    // We check connection in next loop
    delay(10); // Tiny yield
    mb.task();

    if (mb.isConnected(inverterIp)) {
      Serial.println("Huawei: Connected!");
      errorCount = 0;
      changeState(H_READING);
    } else {
      // Failed immediate check? Give it a timeout or go to backoff
      // If we stay in CONNECTING, we might spam.
      // Let's cycle to BACKOFF if this fails immediately.
      Serial.println("Huawei: Connect Refused/Failed.");
      changeState(H_BACKOFF);
    }
    break;

  case H_READING:
    if (!mb.isConnected(inverterIp)) {
      Serial.println("Huawei: Lost Connection.");
      changeState(H_BACKOFF); // Go to backoff to let socket clear
      return;
    }

    if (errorCount > MAX_ERRORS) {
      Serial.println("Huawei: Too many errors. Reconnecting...");
      mb.disconnect(inverterIp);
      changeState(H_BACKOFF);
      return;
    }

    // Scheduler
    if (millis() - lastReadTime > readInterval) {
      lastReadTime = millis();

      // Staggered reads: We can't easily wait between Grid and PV in one loop
      // pass without complex sub-states or blocking. Be simple: Read Grid now.
      // Read PV in next interval? Or Read Grid, then 500ms later Read PV? Let's
      // alternate.
      static int readToggle = 0;

      lastRequestTime = millis();
      if (readToggle == 0) {
// Read Grid (Reg 37113 usually for Active Power, check config)
// config.h says GRID_POWER_REG.
#ifdef DEBUG_HUAWEI
        Serial.println("Huawei: Req Grid");
#endif
        mb.readHreg(inverterIp, GRID_POWER_REG, gridPowerBuf, 2, cbReadPower,
                    INVERTER_SLAVE_ID);
        // Using callback data to indicate type?
        // Emelianov readHreg doesn't easily support passing user data in all
        // versions, but let's check the signature from previous code. Previous
        // code: mb.readHreg(ip, GRID_POWER_REG, gridPowerBuf, 2, cbReadPower,
        // INVERTER_SLAVE_ID); Wait, previous code didn't pass user data!
        // Previous callback: bool cbReadPower(Modbus::ResultCode event,
        // uint16_t transactionId, void *data) But how did it know the type?
        // Previous code used global `lastRequestType`.
        // My new code tries to pass `(void*)0`.
        // The standard library `readHreg` usually signature is:
        // (ip, reg, buf, count, cb, unit)
        // It does NOT have a user-data parameter in standard
        // emelianov/modbus-esp8266. I must revert to using a global
        // `lastRequestType` or similar context approach.

        lastRequestType = 0;
        readToggle = 1;
      } else {
// Read PV (Reg 32080 usually)
#ifdef DEBUG_HUAWEI
        Serial.println("Huawei: Req PV");
#endif
        mb.readHreg(inverterIp, ACTIVE_POWER_REG, gridPowerBuf, 2, cbReadPower,
                    INVERTER_SLAVE_ID);
        lastRequestType = 1;
        readToggle = 0;
      }
    }
    break;

  case H_BACKOFF:
    // Wait 10s before trying again to let EW11 clear sockets
    if (millis() - stateEntryTime > 10000) {
      changeState(H_WIFI_WAIT); // Go check wifi then connect
    }
    break;
  }
}

// Stub for restart (unused but kept if needed)
void restartEW11() {
  // Implementation preserved if needed later
}
