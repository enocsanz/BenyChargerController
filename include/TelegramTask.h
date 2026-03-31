#ifndef TELEGRAM_TASK_H
#define TELEGRAM_TASK_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>

void setupTelegram();
void loopTelegram();
void sendTelegramNotification(String msg);

// extern float required_kwh; REMOVED

extern bool force_charging_mode;  // Manual override
extern float max_price_threshold; // Configurable max price
extern bool manual_logic_trigger; // Trigger Logic run immediately

#endif
