#ifndef SONDA_LINK_H
#define SONDA_LINK_H

#include <Arduino.h>

// ESP-NOW link with the probe next to the water heater. For now a link test:
// counts the messages that arrive, to see whether the link is good enough
// before relying on it for the water temperature.
void setupSondaLink(); // once WiFi is up (ESP-NOW runs on the router's channel)
void loopSondaLink();  // hourly summary to the diagnostic log
String sondaLinkText(); // For Telegram (/espnow)

#endif
