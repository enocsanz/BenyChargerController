// Probe next to the water heater (M5StickC Plus). For now a link test: sends
// one numbered message per second to the main controller and shows on screen
// how many it confirmed, to find a spot where the link holds. Later it will
// also carry the DS18B20 water temperature.
//
// It goes over the home WiFi (UDP): the controller now sits in the pool house,
// out of ESP-NOW range, but next to a mesh point. The controller echoes each
// message back as the confirmation.

#include "SondaPacket.h"
#include "WifiRoam.h"
#include "config.h" // WIFI_SSID, WIFI_PASSWORD, OTA_PASSWORD (../include, not in git)
#include <ArduinoOTA.h>
#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>

// Main controller (StampS3), fixed IP in the router
static const IPAddress MAIN_IP(192, 168, 86, 41);
static const char *SONDA_HOSTNAME = "beny-sonda";

static WiFiUDP udp;
static uint32_t seq = 0, sent = 0, acked = 0;
static uint32_t minSent = 0, minAcked = 0;
static float lastMinutePct = -1;
static bool otaReady = false;

// Water temperature: DS18B20 on G26 (top header). It needs a 4.7k pull-up
// between data and 3.3V (most modules carry it); the pin's internal pull-up
// is enabled as well, though it is too weak to rely on alone.
static const int ONEWIRE_PIN = 26;
static const unsigned long TEMP_INTERVAL = 10000; // a reading every 10 s
static OneWire oneWire(ONEWIRE_PIN);
static DallasTemperature ds(&oneWire);
// Two DS18B20 on the same 1-Wire bus (G26, one 4.7k pull-up for both), told
// apart by their 64-bit address. The bottom one (thermostat's well) is the
// first one ever seen: while it is alone on the bus its address is stored in
// flash, so a second probe added later is known to be the mid-height one
// (inside the old heating coil).
static float waterTemp = NAN;      // bottom: last valid reading
static unsigned long waterAt = 0;  // when it was taken
static float waterTemp2 = NAN;     // mid height: last valid reading
static unsigned long water2At = 0;
static DeviceAddress addrLow;      // bottom probe's address
static bool haveLow = false;

static void loadLowAddr() {
  Preferences p;
  p.begin("sonda", true);
  if (p.getBytesLength("low") == 8) {
    p.getBytes("low", addrLow, 8);
    haveLow = true;
  }
  p.end();
}

static void saveLowAddr(const uint8_t *a) {
  Preferences p;
  p.begin("sonda", false);
  p.putBytes("low", a, 8);
  p.end();
  memcpy(addrLow, a, 8);
  haveLow = true;
}

static bool validTemp(float t) { return t != DEVICE_DISCONNECTED_C && t != 85.0 && t > -20 && t < 110; }
static int sensors = 0;
static float rawTemp = NAN; // last reading as read, for the controller's /sonda
static uint8_t lineFlags = 0; // SONDA_LINE_* from the last check

// Data line check, while no sensor is found: against the weak internal
// pull-down, a 4.7k external pull-up still wins (reads high); with the
// internal pull-up, the line should read high unless shorted to ground.
static void checkLine() {
  uint8_t f = SONDA_LINE_CHECKED;
  pinMode(ONEWIRE_PIN, INPUT_PULLDOWN);
  delay(2);
  if (digitalRead(ONEWIRE_PIN)) f |= SONDA_LINE_PULLUP;
  pinMode(ONEWIRE_PIN, INPUT_PULLUP);
  delay(2);
  if (!digitalRead(ONEWIRE_PIN)) f |= SONDA_LINE_GROUND;
  lineFlags = f;
}

// Non-blocking: request a conversion, collect it on the next call (~750 ms
// later at 12 bits). -127 means no sensor answers; 85.0 is the power-on value
// before the first conversion: neither is a reading.
static void readWater() {
  static bool requested = false;
  static unsigned long requestedAt = 0;
  if (!requested) {
    if (waterAt && millis() - requestedAt < TEMP_INTERVAL) return;
    sensors = ds.getDeviceCount();
    if (sensors == 0) {
      checkLine();
      ds.begin(); // look for it again (a loose contact, hot-plugging)
      pinMode(ONEWIRE_PIN, INPUT_PULLUP);
      sensors = ds.getDeviceCount();
    }
    ds.requestTemperatures();
    requested = true;
    requestedAt = millis();
    return;
  }
  if (millis() - requestedAt < 800) return;
  requested = false;
  float tLow = NAN, tMid = NAN;
  rawTemp = NAN;
  for (int i = 0; i < sensors && i < 4; i++) {
    DeviceAddress a;
    if (!ds.getAddress(a, i)) continue;
    if (!haveLow && sensors == 1) saveLowAddr(a); // alone: it is the bottom one
    float t = ds.getTempC(a);
    // Without a known bottom address (both fitted at once) the first one is it
    bool isLow = haveLow ? memcmp(a, addrLow, 8) == 0 : i == 0;
    if (isLow) {
      rawTemp = t;
      if (validTemp(t)) tLow = t;
    } else if (validTemp(t)) {
      tMid = t;
    }
  }
  // No valid reading for a minute: do not send a stale one
  if (!isnan(tLow)) {
    waterTemp = tLow;
    waterAt = millis();
  } else if (waterAt && millis() - waterAt > 60000) {
    waterTemp = NAN;
  }
  if (!isnan(tMid)) {
    waterTemp2 = tMid;
    water2At = millis();
  } else if (water2At && millis() - water2At > 60000) {
    waterTemp2 = NAN;
  }
}
static unsigned long lastAckAt = 0; // last echo from the controller

// Self-healing. On 03/10 at 22:53 the probe lost the WiFi (-88 dBm) and sent
// nothing for 21 h, although it answered pings once the WiFi came back: the
// automatic reconnection alone is not enough, and the UDP socket can be left
// useless across a reconnection. Escalating recovery, plus a watchdog:
static const unsigned long REJOIN_AFTER = 30000;     // no WiFi: full re-join
static const unsigned long REBOOT_NO_WIFI = 600000;  // no WiFi 10 min: restart
static const unsigned long UDP_RESET_AFTER = 120000; // connected, no echo 2 min: reopen UDP
static const unsigned long REBOOT_NO_ECHO = 600000;  // no echo 10 min: restart
static const int WDT_SECS = 30;

// Echoes are counted whenever they arrive, not only within a short wait: the
// controller answers from its main loop, which can be busy for a second or
// two (Telegram, the Sheets batch). Waiting 300 ms counted those as lost: 64 %
// next to the router, at -60 dBm. Any echo of one of the last ACK_WINDOW
// messages counts, once.
static const uint32_t ACK_WINDOW = 32;
static uint32_t ackedSeq[ACK_WINDOW]; // seq confirmed in each slot

static void sendOne() {
  SondaPacket p;
  p.magic = SONDA_MAGIC;
  p.seq = ++seq;
  p.uptime = millis() / 1000;
  p.temp = waterTemp; // NAN without a valid reading
  p.rssi = (int8_t)WiFi.RSSI();
  strncpy(p.fw, __DATE__ " " __TIME__, sizeof(p.fw));
  p.sensors = sensors;
  p.rawTemp = rawTemp;
  p.line = lineFlags;
  p.temp2 = waterTemp2;
  udp.beginPacket(MAIN_IP, SONDA_UDP_PORT);
  udp.write((uint8_t *)&p, sizeof(p));
  udp.endPacket();
}

static void readEchoes() {
  for (int n = udp.parsePacket(); n > 0; n = udp.parsePacket()) {
    if (n != sizeof(SondaPacket)) {
      udp.flush();
      continue;
    }
    SondaPacket r;
    udp.read((uint8_t *)&r, sizeof(r));
    if (r.magic != SONDA_MAGIC || r.seq == 0 || r.seq > seq || seq - r.seq >= ACK_WINDOW) continue;
    uint32_t &slot = ackedSeq[r.seq % ACK_WINDOW];
    if (slot != r.seq) { // first echo of this message
      slot = r.seq;
      acked++;
      lastAckAt = millis();
    }
  }
}

static void setupOta() {
  ArduinoOTA.setHostname(SONDA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("Actualizando...");
  });
  ArduinoOTA.begin();
  otaReady = true;
}

static void draw() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK);
  if (WiFi.status() != WL_CONNECTED) {
    M5.Lcd.println("Sonda termo");
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.println("SIN WIFI");
    return;
  }
  // Water temperatures, big: bottom (well) and, if fitted, mid height (coil)
  M5.Lcd.setTextSize(3);
  if (isnan(waterTemp)) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println(sensors ? "Sonda sin dato" : "Sin sonda");
  } else {
    M5.Lcd.setTextColor(CYAN, BLACK);
    M5.Lcd.printf("Ab %.1fC\n", waterTemp);
  }
  if (!isnan(waterTemp2)) {
    M5.Lcd.setTextColor(ORANGE, BLACK);
    M5.Lcd.printf("Me %.1fC\n", waterTemp2);
  }
  M5.Lcd.setTextSize(2);
  // WiFi signal: green >= -67, yellow >= -75, red below
  long rssi = WiFi.RSSI();
  M5.Lcd.setTextColor(rssi >= -67 ? GREEN : rssi >= -75 ? YELLOW : RED, BLACK);
  M5.Lcd.printf("WiFi %ld dBm\n", rssi);
  M5.Lcd.setTextColor(WHITE, BLACK);
  if (lastMinutePct >= 0) {
    uint16_t c = lastMinutePct >= 80 ? GREEN : lastMinutePct >= 50 ? YELLOW : RED;
    M5.Lcd.setTextColor(c, BLACK);
    M5.Lcd.printf("Enlace %.0f%%\n", lastMinutePct);
    M5.Lcd.setTextColor(WHITE, BLACK);
  } else {
    M5.Lcd.println("Enlace: midiendo");
  }
  M5.Lcd.setTextSize(1);
  M5.Lcd.printf("%s %s\n", WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str());
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(60); // 0-100 on the Plus (9 left it almost off)
  Serial.begin(115200);

  // Mesh network: join the strongest point, not the first one found
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  loadLowAddr();
  ds.begin();
  ds.setWaitForConversion(false); // readWater() collects it later
  pinMode(ONEWIRE_PIN, INPUT_PULLUP);
  sensors = ds.getDeviceCount();

  draw();
  udp.begin(SONDA_UDP_PORT);
  lastAckAt = millis();

  esp_task_wdt_init(WDT_SECS, true);
  esp_task_wdt_add(NULL);
}

static void restart(const char *why) {
  Serial.printf("Reinicio: %s\n", why);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.printf("Reinicio:\n%s", why);
  delay(1000);
  ESP.restart();
}

// Escalating recovery of the WiFi and of the UDP link
static void heal() {
  static bool wasConnected = false;
  static unsigned long downSince = 0, lastRejoin = 0, lastUdpReset = 0;
  bool connected = WiFi.status() == WL_CONNECTED;

  if (!connected) {
    if (downSince == 0) downSince = lastRejoin = millis();
    if (millis() - lastRejoin > REJOIN_AFTER) {
      lastRejoin = millis();
      Serial.println("WiFi: reconexion completa");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // any point, strongest first (no BSSID)
    }
    if (millis() - downSince > REBOOT_NO_WIFI) restart("10 min sin WiFi");
    wasConnected = false;
    return;
  }
  downSince = 0;

  if (!wasConnected) { // just (re)connected: fresh UDP socket
    wasConnected = true;
    udp.stop();
    udp.begin(SONDA_UDP_PORT);
    lastAckAt = lastUdpReset = millis();
    Serial.println("WiFi conectada: UDP reabierto");
    return;
  }

  unsigned long silent = millis() - lastAckAt;
  if (silent > UDP_RESET_AFTER && millis() - lastUdpReset > UDP_RESET_AFTER) {
    lastUdpReset = millis();
    Serial.println("Sin confirmaciones: UDP reabierto");
    udp.stop();
    udp.begin(SONDA_UDP_PORT);
  }
  if (silent > REBOOT_NO_ECHO) restart("10 min sin respuesta");
}

void loop() {
  static unsigned long lastSend = 0, lastMinute = 0, lastDraw = 0;
  esp_task_wdt_reset();
  heal();
  readWater();

  if (WiFi.status() == WL_CONNECTED) {
    if (!otaReady) setupOta();
    ArduinoOTA.handle();
    wifiRoamLoop(WIFI_SSID, WIFI_PASSWORD, [](const String &m) { Serial.println(m); });

    readEchoes();
    if (millis() - lastSend >= 1000) {
      lastSend = millis();
      sent++;
      sendOne();
    }
  }

  if (millis() - lastMinute >= 60000) {
    lastMinute = millis();
    uint32_t s = sent - minSent;
    lastMinutePct = s ? 100.0 * (acked - minAcked) / s : -1;
    minSent = sent;
    minAcked = acked;
    Serial.printf("Ultimo minuto: %.0f%% | total %u/%u | WiFi %d dBm\n", lastMinutePct, acked,
                  sent, WiFi.RSSI());
  }

  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    draw();
  }
  delay(5);
}
