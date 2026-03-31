#ifndef HUAWEI_TASK_H
#define HUAWEI_TASK_H

#include <Arduino.h>
#include <ModbusIP_ESP8266.h>
#include <WiFi.h>

void setupHuawei();
void loopHuawei();

extern int32_t current_grid_power; // Positive = Import, Negative = Export
extern int32_t current_pv_power;

#endif
