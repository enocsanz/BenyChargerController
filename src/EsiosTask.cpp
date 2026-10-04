#include "EsiosTask.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// Two hourly prices from ESIOS (REE), one day at a time:
//  - 1001: PVPC 2.0TD, what the grid energy costs (Peninsula)
//  - 1739: what exported energy is paid under the simplified compensation for
//          PVPC (Spain). It can go (slightly) negative on sunny low-demand days.
PriceState esios_prices;
PriceState esios_surplus;
static const char *URL_PVPC =
    "https://api.esios.ree.es/indicators/1001?geo_ids[]=8741&time_trunc=hour";
static const char *URL_SURPLUS = "https://api.esios.ree.es/indicators/1739?time_trunc=hour";
const unsigned long priceUpdateInterval = 3600000; // 1 hour

void setupEsios() {
  // Initial fetch
  loopEsios();
}

// Fetches one indicator for today into st. True if it got the values.
static bool fetchIndicator(const char *baseUrl, PriceState &st, const struct tm &timeinfo,
                           const char *name) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  char startDate[20], endDate[20];
  strftime(startDate, sizeof(startDate), "%Y-%m-%dT00:00", &timeinfo);
  strftime(endDate, sizeof(endDate), "%Y-%m-%dT23:59", &timeinfo);
  String url = String(baseUrl) + "&start_date=" + startDate + "&end_date=" + endDate;

  bool ok = false;
  if (http.begin(client, url)) {
    http.addHeader("Accept", "application/json; application/vnd.esios-api-v2+json");
    http.addHeader("Content-Type", "application/json");
    if (String(ESIOS_TOKEN) != "YOUR_ESIOS_TOKEN") {
      http.addHeader("x-api-key", ESIOS_TOKEN);
    }

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      DynamicJsonDocument doc(8192);
      DeserializationError error = deserializeJson(doc, http.getStream());
      if (!error) {
        for (int i = 0; i < 24; i++) st.valid[i] = false;
        JsonArray values = doc["indicator"]["values"];
        for (JsonObject v : values) {
          // "datetime":"2023-10-27T00:00:00.000+02:00"
          const char *dt = v["datetime"];
          int hour = atoi(dt + 11);
          if (hour >= 0 && hour < 24) {
            st.prices[hour] = v["value"].as<float>() / 1000.0; // EUR/MWh -> EUR/kWh
            st.valid[hour] = true;
          }
        }
        st.lastUpdate = millis();
        st.yday = timeinfo.tm_yday;
        ok = true;
        Serial.printf("ESIOS %s updated\n", name);
      } else {
        Serial.printf("ESIOS %s JSON parse failed: %s\n", name, error.c_str());
      }
    } else {
      Serial.printf("ESIOS %s HTTP failed: %d\n", name, httpCode);
    }
    http.end();
  }
  return ok;
}

static bool isDue(const PriceState &st, const struct tm *now) {
  bool newDay = now && now->tm_yday != st.yday;
  return st.lastUpdate == 0 || millis() - st.lastUpdate > priceUpdateInterval || newDay;
}

void loopEsios() {
  // Also refetch as soon as the day changes: each table holds a single day, and
  // right after midnight it would otherwise serve yesterday's prices.
  struct tm now;
  bool haveTime = timeNow(&now);
  bool duePvpc = isDue(esios_prices, haveTime ? &now : nullptr);
  bool dueSurplus = isDue(esios_surplus, haveTime ? &now : nullptr);

  // A failed fetch is retried once a minute, not on every loop: each attempt
  // is a TLS handshake that can stall the loop for seconds.
  static unsigned long lastAttempt = 0;
  static bool attempted = false;
  if (!(duePvpc || dueSurplus)) return;
  if (attempted && millis() - lastAttempt < 60000) return;
  attempted = true;
  lastAttempt = millis();

  if (WiFi.status() != WL_CONNECTED) return;
  if (!haveTime) {
    Serial.println("Failed to obtain time");
    return;
  }
  if (duePvpc) fetchIndicator(URL_PVPC, esios_prices, now, "PVPC");
  if (dueSurplus) fetchIndicator(URL_SURPLUS, esios_surplus, now, "excedentes");
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

// NAN when unknown: unlike the PVPC, a negative value is a real price here
float getCurrentSurplusPrice() {
  struct tm t;
  if (!timeNow(&t) || t.tm_yday != esios_surplus.yday || !esios_surplus.valid[t.tm_hour]) return NAN;
  return esios_surplus.prices[t.tm_hour];
}

bool surplusPriceNegative() {
  float p = getCurrentSurplusPrice();
  return !isnan(p) && p < 0;
}
