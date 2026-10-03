// Probe next to the water heater (M5StickC Plus). For now a link test: sends
// one numbered message per second to the main controller and shows on screen
// how many it confirmed, to find a spot where the link holds. Later it will
// also carry the DS18B20 water temperature.
//
// It goes over the home WiFi (UDP): the controller now sits in the pool house,
// out of ESP-NOW range, but next to a mesh point. The controller echoes each
// message back as the confirmation.

#include "SondaPacket.h"
#include "config.h" // WIFI_SSID, WIFI_PASSWORD, OTA_PASSWORD (../include, not in git)
#include <ArduinoOTA.h>
#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// Main controller (StampS3), fixed IP in the router
static const IPAddress MAIN_IP(192, 168, 86, 41);
static const char *SONDA_HOSTNAME = "beny-sonda";

static WiFiUDP udp;
static uint32_t seq = 0, sent = 0, acked = 0;
static uint32_t minSent = 0, minAcked = 0;
static float lastMinutePct = -1;
static bool otaReady = false;

// Sends one message and waits for the controller's echo
static bool sendOne() {
  SondaPacket p;
  p.magic = SONDA_MAGIC;
  p.seq = ++seq;
  p.uptime = millis() / 1000;
  p.temp = NAN; // no sensor yet
  udp.beginPacket(MAIN_IP, SONDA_UDP_PORT);
  udp.write((uint8_t *)&p, sizeof(p));
  if (!udp.endPacket()) return false;

  unsigned long t0 = millis();
  while (millis() - t0 < 300) {
    int n = udp.parsePacket();
    if (n == sizeof(SondaPacket)) {
      SondaPacket r;
      udp.read((uint8_t *)&r, sizeof(r));
      if (r.magic == SONDA_MAGIC && r.seq == p.seq) return true;
    } else if (n > 0) {
      udp.flush();
    }
    delay(2);
  }
  return false;
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
  // WiFi signal: green >= -67, yellow >= -75, red below
  long rssi = WiFi.RSSI();
  M5.Lcd.setTextColor(rssi >= -67 ? GREEN : rssi >= -75 ? YELLOW : RED, BLACK);
  M5.Lcd.printf("WiFi %ld dBm\n", rssi);
  M5.Lcd.setTextColor(WHITE, BLACK);
  if (lastMinutePct >= 0) {
    uint16_t c = lastMinutePct >= 80 ? GREEN : lastMinutePct >= 50 ? YELLOW : RED;
    M5.Lcd.setTextColor(c, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.printf("%3.0f%%\n", lastMinutePct);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE, BLACK);
  } else {
    M5.Lcd.println("midiendo...");
  }
  M5.Lcd.printf("Total %.0f%% %u\n", sent ? 100.0 * acked / sent : 0, sent);
  M5.Lcd.setTextSize(1);
  M5.Lcd.printf("%s %s\n", WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str());
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(9);
  Serial.begin(115200);

  // Mesh network: join the strongest point, not the first one found
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  draw();
  udp.begin(SONDA_UDP_PORT);
}

void loop() {
  static unsigned long lastSend = 0, lastMinute = 0, lastDraw = 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (!otaReady) setupOta();
    ArduinoOTA.handle();

    if (millis() - lastSend >= 1000) {
      lastSend = millis();
      sent++;
      if (sendOne()) acked++;
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
