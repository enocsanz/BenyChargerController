#ifndef TELEGRAM_TASK_H
#define TELEGRAM_TASK_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>

void setupTelegram();
void loopTelegram();
// Queues a message; loopTelegram() performs the actual (blocking) send.
void sendTelegramNotification(String msg);

extern float max_price_threshold; // Configurable max price
extern bool manual_logic_trigger; // Trigger Logic run immediately

#endif
