#ifndef ESIOS_TASK_H
#define ESIOS_TASK_H

#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>


struct PriceState {
  float prices[24];
  bool valid[24];
  unsigned long lastUpdate;
  int yday; // tm_yday the prices belong to, -1 = none yet

  PriceState() {
    yday = -1;
    for (int i = 0; i < 24; i++) {
      prices[i] = 0.0;
      valid[i] = false;
    }
    lastUpdate = 0;
  }
};

extern PriceState esios_prices;  // PVPC: what grid energy costs
extern PriceState esios_surplus; // what exported energy is paid (simplified compensation)

// Price paid for exported energy this hour (EUR/kWh), NAN if unknown. It can
// be negative (exporting then costs money). Only recorded and shown: no action.
float getCurrentSurplusPrice();
bool surplusPriceNegative();

void setupEsios();
void loopEsios();
float getCurrentPrice();
// Local time right now, without waiting; false until NTP has set the clock.
// Use this instead of getLocalTime(&t, 0), which fails at random (see .cpp).
bool timeNow(struct tm *t);
// Today's price for an hour (0-23), -1 if unknown
float getPriceAt(int hour);

#endif
