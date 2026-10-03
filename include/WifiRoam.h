#ifndef WIFI_ROAM_H
#define WIFI_ROAM_H

// Roaming between the points of a mesh network (several Google Wifi with the
// same SSID). The ESP32 never changes access point on its own: once joined it
// stays, however weak. And the scan at boot sometimes misses the closest
// point: after an OTA restart the controller joined a far one at -81 dBm with
// a point right next to it (-46 the boots before).
//
// While the signal is weak, every ROAM_INTERVAL it scans in the background and
// moves to a point at least ROAM_MARGIN dB better. Shared by the controller and
// the probe: call wifiRoamLoop() from loop() while connected.

#include <Arduino.h>
#include <WiFi.h>

static const int ROAM_WEAK = -70;                     // dBm: only look below this
static const int ROAM_MARGIN = 10;                    // dB better to move
static const unsigned long ROAM_FIRST = 60000;        // first check, after boot
static const unsigned long ROAM_INTERVAL = 300000;    // then every 5 min

// log: called with a line describing each move (or nullptr)
inline void wifiRoamLoop(const char *ssid, const char *pass, void (*log)(const String &)) {
  static unsigned long lastCheck = 0;
  static bool scanning = false;
  static unsigned long bootAt = millis();

  if (WiFi.status() != WL_CONNECTED) {
    scanning = false;
    return;
  }

  if (!scanning) {
    unsigned long wait = lastCheck == 0 ? ROAM_FIRST : ROAM_INTERVAL;
    unsigned long since = lastCheck == 0 ? millis() - bootAt : millis() - lastCheck;
    if (since < wait) return;
    lastCheck = millis();
    if (WiFi.RSSI() >= ROAM_WEAK) return;
    // Background scan: the loop keeps running, the link only pauses briefly
    WiFi.scanNetworks(true, false, false, 120);
    scanning = true;
    return;
  }

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanning = false;
  if (n <= 0) {
    WiFi.scanDelete();
    return;
  }

  int best = -1;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
  }
  int cur = WiFi.RSSI();
  if (best >= 0 && WiFi.RSSI(best) >= cur + ROAM_MARGIN && WiFi.BSSIDstr(best) != WiFi.BSSIDstr()) {
    uint8_t bssid[6];
    memcpy(bssid, WiFi.BSSID(best), 6);
    int ch = WiFi.channel(best);
    if (log) {
      log("Cambio de punto de acceso: " + WiFi.BSSIDstr() + " (" + String(cur) + " dBm) -> " +
          WiFi.BSSIDstr(best) + " (" + String(WiFi.RSSI(best)) + " dBm)");
    }
    WiFi.scanDelete();
    WiFi.begin(ssid, pass, ch, bssid);
    return;
  }
  WiFi.scanDelete();
}

#endif
