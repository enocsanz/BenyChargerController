#include "EsiosTask.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

PriceState esios_prices;
const char *esios_url =
    "https://api.esios.ree.es/indicators/1001?geo_ids[]=8741&time_trunc=hour";
const unsigned long priceUpdateInterval = 3600000; // 1 hour

void setupEsios() {
  // Initial fetch
  loopEsios();
}

void loopEsios() {
  // Also refetch as soon as the day changes: the table holds a single day, and
  // right after midnight it would otherwise serve yesterday's prices.
  struct tm now;
  bool newDay = timeNow(&now) && now.tm_yday != esios_prices.yday;
  bool due = millis() - esios_prices.lastUpdate > priceUpdateInterval ||
             esios_prices.lastUpdate == 0 || newDay;

  // A failed fetch is retried once a minute, not on every loop: each attempt
  // is a TLS handshake that can stall the loop for seconds.
  static unsigned long lastAttempt = 0;
  static bool attempted = false;
  if (due && attempted && millis() - lastAttempt < 60000) due = false;

  if (due) {
    attempted = true;
    lastAttempt = millis();
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;

      // Calculate start/end dates for URL if needed, or rely on API defaults
      // (usually returns current day/next day) For simplicity, we use the
      // default which gives today/tomorrow usually Better implementation
      // involves proper date handling like in reference code

      // Using simplified URL for now, strictly mirroring reference
      // Reference constructed URL with dates. We should probably do that if we
      // want robust data. But for MVP, let's try the direct URL first or add
      // Time retrieval.

      // To do it right, we need time.
      struct tm timeinfo;
      if (!timeNow(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
      }

      char startDate[20], endDate[20];
      strftime(startDate, sizeof(startDate), "%Y-%m-%dT00:00", &timeinfo);
      strftime(endDate, sizeof(endDate), "%Y-%m-%dT23:59", &timeinfo);

      String url = String(esios_url) + "&start_date=" + startDate +
                   "&end_date=" + endDate;

      if (http.begin(client, url)) {
        http.addHeader("Accept",
                       "application/json; application/vnd.esios-api-v2+json");
        http.addHeader("Content-Type", "application/json");
        if (String(ESIOS_TOKEN) != "YOUR_ESIOS_TOKEN") {
          http.addHeader("x-api-key", ESIOS_TOKEN);
        }

        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
          DynamicJsonDocument doc(8192); // Increased buffer
          DeserializationError error = deserializeJson(doc, http.getStream());

          if (!error) {
            for (int i = 0; i < 24; i++) esios_prices.valid[i] = false;
            JsonArray values = doc["indicator"]["values"];
            for (JsonObject v : values) {
              // "datetime":"2023-10-27T00:00:00.000+02:00"
              const char *dt = v["datetime"];
              int hour = atoi(dt + 11);
              float price = v["value"].as<float>() / 1000.0; // convert to €/kWh

              if (hour >= 0 && hour < 24) {
                esios_prices.prices[hour] = price;
                esios_prices.valid[hour] = true;
              }
            }
            esios_prices.lastUpdate = millis();
            esios_prices.yday = timeinfo.tm_yday;
            Serial.println("ESIOS prices updated");
          } else {
            Serial.print("ESIOS JSON parse failed: ");
            Serial.println(error.c_str());
          }
        } else {
          Serial.printf("ESIOS HTTP failed: %d\n", httpCode);
        }
        http.end();
      }
    }
  }
}

// timeNow(&t) is not "no wait": it loops while millis() - start <= 0,
// so when millis() ticks between its two reads it never looks at the clock and
// returns false with the time perfectly set. That made the price read as
// unknown for a second every few minutes, and the heater and the pool pump
// switched on and off with it. Plain time() has no such race. (The default
// timeout is no option either: without NTP it waits 5s on every call.)
bool timeNow(struct tm *t) {
  time_t now = time(nullptr);
  localtime_r(&now, t);
  return t->tm_year > (2016 - 1900);
}

float getPriceAt(int hour) {
  struct tm timeinfo;
  if (timeNow(&timeinfo) && timeinfo.tm_yday == esios_prices.yday && hour >= 0 &&
      hour < 24 && esios_prices.valid[hour]) {
    return esios_prices.prices[hour];
  }
  return -1.0;
}

float getCurrentPrice() {
  struct tm timeinfo;
  if (!timeNow(&timeinfo)) return -1.0;
  return getPriceAt(timeinfo.tm_hour);
}
