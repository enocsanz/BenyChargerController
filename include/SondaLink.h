#ifndef SONDA_LINK_H
#define SONDA_LINK_H

#include <Arduino.h>

// Link with the probe next to the water heater (M5StickC Plus), over the home
// WiFi (UDP) or ESP-NOW: counts the messages that arrive and keeps the last
// reading (water temperature, its WiFi signal, firmware).
void setupSondaLink(); // once WiFi is up (UDP port + ESP-NOW on the router's channel)
void loopSondaLink();  // hourly summary to the diagnostic log
String sondaLinkText();    // For Telegram: /sonda, everything
String sondaShortText();   // one line for /status
String sondaVersionText(); // probe firmware and IP, for /version
String sondaWifiText();    // probe WiFi signal, for /wifi
float sondaWaterTemp();    // water temperature, NAN without a fresh reading

#endif
