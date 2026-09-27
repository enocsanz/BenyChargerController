#ifndef TERMO_TASK_H
#define TERMO_TASK_H

#include <Arduino.h>

// Electric water heater (ACS) behind a Tongou metering relay, driven over
// Tuya local. The heater keeps its own mechanical thermostat: the relay only
// enables or cuts it.

// Modes: AUTO follows price and overload rules; ON ignores the price (the
// overload protection still applies); OFF keeps the relay off.
enum TermoMode { TERMO_AUTO = 0, TERMO_ON = 1, TERMO_OFF = 2 };

// Why the relay is where it is
enum TermoReason {
  TR_ENABLED,  // relay on, the thermostat decides
  TR_PRICE,    // cut: price above termo_max_price
  TR_OVERLOAD, // cut: grid over the contracted power, waiting for room
  TR_MANUAL,   // cut: TERMO_OFF
  TR_OFFLINE   // no session with the relay
};

struct TermoStatus {
  bool online;
  bool relayOn;
  float power; // W, as measured by the relay
  TermoReason reason;
};

void setupTermo();
void loopTermo();     // Relay I/O. Every loop, only with WiFi.
void runTermoLogic(); // Decision. Once per second, with the DLB.

TermoStatus getTermoStatus();
String termoStatusText(); // For Telegram
void setTermoMode(int mode);
void setTermoMaxPrice(float price);

extern int termo_mode;
extern float termo_max_price;

#endif
