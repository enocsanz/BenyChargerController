#include "EnvSensor.h"
#include "GoogleSheetsTask.h"
#include <Preferences.h>
#include <Wire.h>
#include <esp_task_wdt.h>

extern void sendTelegramNotification(String msg);

static const uint8_t SHT30_ADDR = 0x44;
static const uint8_t QMP6988_ADDR = 0x70;
static const unsigned long READ_INTERVAL = 30000;
// Slow bus: the StampS3 has no Grove socket and maybe no pull-ups, so the
// lines may hang on the ESP32's weak internal ones (~45k). At 100 kHz the
// SHT30 answered only now and then; it works at any speed down to 0.
static const uint32_t I2C_HZ = 20000;

// Free StampS3 pins that may carry the I2C wires. Left out: G0 (button and
// boot), G3/G45/G46 (boot strapping), G19/G20 (USB), G21 (LED), G26-G37 (flash).
static const int CANDIDATES[] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 39, 40, 41, 42, 43, 44};

static int sdaPin = -1, sclPin = -1;
static bool ok = false;
static float temp = NAN, hum = NAN;
static unsigned long lastRead = 0;

static bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool tryPins(int sda, int scl) {
  Wire.end();
  if (!Wire.begin(sda, scl, I2C_HZ)) return false;
  Wire.setTimeOut(20);
  return probe(SHT30_ADDR);
}

static String lastError = ""; // why the last read failed, for /status

static int lastCmdCode = -1; // Wire.endTransmission() of the last command

// Sensirion CRC-8 (poly 0x31, init 0xFF) over a 2-byte word
static uint8_t crc8(const uint8_t *data) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < 2; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

static bool command(uint8_t hi, uint8_t lo) {
  Wire.beginTransmission(SHT30_ADDR);
  Wire.write(hi);
  Wire.write(lo);
  lastCmdCode = Wire.endTransmission();
  return lastCmdCode == 0;
}

// For /i2c: idle line levels, what answers on the bus, last command result.
// A line stuck low makes EVERY address look present, which is why a probe
// can "find" the SHT30 that then rejects its commands.
String envI2cDiag() {
  if (sdaPin < 0) return "🔌 I2C: no hay pines detectados";
  String msg = "🔌 I2C en SDA G" + String(sdaPin) + ", SCL G" + String(sclPin) + "\n";
  Wire.end();
  pinMode(sdaPin, INPUT);
  pinMode(sclPin, INPUT);
  delay(2);
  int sda = digitalRead(sdaPin), scl = digitalRead(sclPin);
  msg += "   En reposo: SDA=" + String(sda) + ", SCL=" + String(scl) +
         (sda && scl ? " (bien)" : " (deberian ser 1: linea a masa o sin resistencias)") + "\n";
  Wire.begin(sdaPin, sclPin, I2C_HZ);
  Wire.setTimeOut(20);
  String found;
  int count = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (probe(a)) {
      count++;
      if (count <= 12) found += " 0x" + String(a, HEX);
    }
  }
  msg += "   Responden " + String(count) + " direcciones:" + (count ? found : String(" ninguna"));
  if (count > 12) msg += " ... (todas: linea bloqueada)";
  msg += "\n   Esperadas: 0x44 (SHT30) y 0x70 (QMP6988)";
  bool m = command(0x24, 0x00);
  msg += "\n   Orden de medida: " + String(m ? "aceptada" : "rechazada") + " (codigo " +
         String(lastCmdCode) + ")";
  return msg;
}

// Single-shot measurement, high repeatability, no clock stretching (0x2400).
// The SHT30 NACKs the read until the measurement is done (up to 15 ms): poll
// a few times instead of a single read at a fixed delay.
static bool readSht30() {
  bool sent = false;
  for (int i = 0; i < 3 && !sent; i++) {
    if (i) delay(5);
    sent = command(0x24, 0x00);
  }
  if (!sent) {
    lastError = "no acepta la orden de medida (codigo " + String(lastCmdCode) + ")";
    return false;
  }
  int got = 0;
  for (int i = 0; i < 8 && got != 6; i++) {
    delay(10);
    got = Wire.requestFrom((int)SHT30_ADDR, 6);
  }
  if (got != 6) {
    lastError = "no devuelve la medida (" + String(got) + " bytes)";
    return false;
  }
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  // Each word comes with a CRC-8: garbage from a bad bus (seen: -45.0 C and
  // 100 %, i.e. all zeros and all ones) must not pass as a reading
  if (crc8(d) != d[2] || crc8(d + 3) != d[5]) {
    lastError = "datos corruptos (CRC)";
    return false;
  }
  lastError = "";
  uint16_t rt = (d[0] << 8) | d[1], rh = (d[3] << 8) | d[4];
  temp = -45.0 + 175.0 * rt / 65535.0;
  hum = 100.0 * rh / 65535.0;
  return true;
}

void setupEnvSensor() {
  Preferences p;
  p.begin("beny", true);
  sdaPin = p.getInt("env_sda", -1);
  sclPin = p.getInt("env_scl", -1);
  p.end();

  // Pins found on an earlier boot, then the documented wiring (SDA G13, SCL G15)
  if (sdaPin >= 0 && tryPins(sdaPin, sclPin)) {
    ok = true;
  } else if (tryPins(13, 15)) {
    ok = true;
    sdaPin = 13;
    sclPin = 15;
    Preferences w;
    w.begin("beny", false);
    w.putInt("env_sda", sdaPin);
    w.putInt("env_scl", sclPin);
    w.end();
  } else {
    // Try every pair of candidate pins (both orders: SDA/SCL may be swapped)
    ok = false;
    int n = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);
    for (int i = 0; i < n && !ok; i++) {
      esp_task_wdt_reset();
      for (int j = 0; j < n && !ok; j++) {
        if (i == j) continue;
        if (tryPins(CANDIDATES[i], CANDIDATES[j])) {
          ok = true;
          sdaPin = CANDIDATES[i];
          sclPin = CANDIDATES[j];
        }
      }
    }
    if (!ok) {
      Wire.end();
      logEvent("CASETA", "Sensor ENV III no encontrado en los pines libres");
      sendTelegramNotification("🌡️ Sensor ENV III: no encontrado. Revisa las conexiones "
                               "(5V, GND, SDA, SCL) o dime a que pines lo has conectado.");
      return;
    }
    Preferences w;
    w.begin("beny", false);
    w.putInt("env_sda", sdaPin);
    w.putInt("env_scl", sclPin);
    w.end();
  }

  bool qmp = probe(QMP6988_ADDR);
  command(0x30, 0xA2); // soft reset
  delay(5);
  readSht30();
  lastRead = millis();
  String msg = "Sensor ENV III en SDA G" + String(sdaPin) + ", SCL G" + String(sclPin) +
               " | SHT30 OK" + (qmp ? ", QMP6988 OK" : ", QMP6988 no responde") +
               (isnan(temp) ? String("") : " | " + String(temp, 1) + " C, " + String(hum, 0) + " %");
  logEvent("CASETA", msg);
  sendTelegramNotification("🌡️ " + msg);
}

void loopEnvSensor() {
  if (!ok || millis() - lastRead < READ_INTERVAL) return;
  lastRead = millis();
  if (!readSht30()) {
    temp = hum = NAN;
  }
}

bool envSensorOk() { return ok; }
float envTemp() { return temp; }
float envHumidity() { return hum; }

String envText() {
  if (!ok) return "🌡️ Caseta: sin sensor";
  if (isnan(temp)) {
    return "🌡️ Caseta: sensor sin lectura (SDA G" + String(sdaPin) + ", SCL G" + String(sclPin) +
           (lastError.length() ? ": " + lastError : String("")) + ")";
  }
  return "🌡️ Caseta: " + String(temp, 1) + " C, " + String(hum, 0) + " % humedad";
}
