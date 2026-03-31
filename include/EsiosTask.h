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

  PriceState() {
    for (int i = 0; i < 24; i++) {
      prices[i] = 0.0;
      valid[i] = false;
    }
    lastUpdate = 0;
  }
};

extern PriceState esios_prices;

void setupEsios();
void loopEsios();
float getCurrentPrice();

#endif
