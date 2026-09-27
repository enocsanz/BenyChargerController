#ifndef HUAWEI_TASK_H
#define HUAWEI_TASK_H

#include <Arduino.h>
#include <ModbusIP_ESP8266.h>
#include <WiFi.h>

void setupHuawei();
void loopHuawei();

extern int32_t current_grid_power; // Positive = Import, Negative = Export
extern int32_t current_pv_power;
// Bumped on every successful grid read, so the DLB can tell a fresh sample
// from a repeated one (reads slow down to 10s when the inverter lags).
extern uint32_t grid_sample_count;

#endif
