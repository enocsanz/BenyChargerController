#ifndef PISCINA_TASK_H
#define PISCINA_TASK_H

#include <Arduino.h>

// Pool pump + salt chlorinator behind a Tongou metering relay (Tuya local).
// Runs on solar surplus, with a daily maximum and a minimum that is topped up
// in the cheapest night hours when the sun did not provide it.

// AUTO follows the rules; ON and OFF force the relay.
enum PiscinaMode { PISCINA_AUTO = 0, PISCINA_ON = 1, PISCINA_OFF = 2 };

enum PiscinaReason {
  PR_SOLAR,      // running on solar surplus
  PR_FILL,       // running to top up yesterday's minimum, cheap night hour
  PR_WAITING,    // stopped, waiting for enough sun
  PR_DONE,       // stopped, today's maximum reached
  PR_MANUAL_ON,  // PISCINA_ON
  PR_MANUAL_OFF, // PISCINA_OFF
  PR_OFFLINE     // no session with the relay
};

struct PiscinaStatus {
  bool online;
  bool relayOn;
  float power;        // W, as measured by the relay
  PiscinaReason reason;
  float hoursToday;   // run time today (h), top-up runs excluded
  float maxHours;     // today's maximum (h)
  float minHours;     // today's minimum (h)
  float deficitHours; // yesterday's shortfall still to top up tonight (h)
  float surplus;      // W, averaged solar surplus available for the pump
};

void setupPiscina();
void loopPiscina();     // Relay I/O. Every loop, only with WiFi.
void runPiscinaLogic(); // Decision. Once per second, with the DLB.

PiscinaStatus getPiscinaStatus();
String piscinaStatusText(); // For Telegram
void setPiscinaMode(int mode);
// Sets this month's maximum and minimum hours (persisted)
bool setPiscinaHours(float maxHours, float minHours);

extern int piscina_mode;

#endif
