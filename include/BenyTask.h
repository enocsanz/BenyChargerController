#ifndef BENY_TASK_H
#define BENY_TASK_H

#include <Arduino.h>
#include <WiFiUdp.h>

struct BenyData {
  bool online;
  float power;       // W
  float total_kwh;   // kWh
  float voltage;     // V
  float current;     // A
  String status;     // CHARGING, STANDBY, etc.
  bool allow_charge; // If we have enabled it (tracking our commands)
};

void setupBeny();
void loopBeny();

// Commands
void benyStartCharge();
void benyStopCharge();
void benyPollDevices();
void benySetCurrent(int amps);
BenyData getBenyData();

#endif
