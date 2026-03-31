#include "EsiosTask.h"
#include <WiFi.h>

PriceState esios_prices;
const char *esios_url =
    "https://api.esios.ree.es/indicators/1001?geo_ids[]=8741&time_trunc=hour";
const unsigned long priceUpdateInterval = 3600000; // 1 hour

void setupEsios() {
  // Initial fetch
  loopEsios();
}

void loopEsios() {
  if (millis() - esios_prices.lastUpdate > priceUpdateInterval ||
      esios_prices.lastUpdate == 0) {
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
      if (!getLocalTime(&timeinfo)) {
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

float getCurrentPrice() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int h = timeinfo.tm_hour;
    if (h >= 0 && h < 24 && esios_prices.valid[h]) {
      return esios_prices.prices[h];
    }
  }
  return -1.0;
}
