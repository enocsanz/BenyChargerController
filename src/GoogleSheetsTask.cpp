#include "GoogleSheetsTask.h"
#include "BenyTask.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Externs for data to log
extern float getCurrentPrice();
extern int32_t current_grid_power;
extern int32_t current_pv_power;
extern int charging_mode;
extern bool auto_paused;

unsigned long lastSheetsCheck = 0;
bool sent_this_hour = false;

void setupGoogleSheets() { Serial.println("GoogleSheets: Initialized."); }

void loopGoogleSheets() {
  // Check every 10 seconds to catch the minute 0
  if (millis() - lastSheetsCheck > 10000) {
    lastSheetsCheck = millis();

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
      return;

    // Condition: Hourly logging (Minute == 0)
    // Removed relay_state check as user removed relay integration.

    if (timeinfo.tm_min == 0) {
      if (!sent_this_hour) {
        Serial.println("GoogleSheets: Sending hourly log...");

        if (WiFi.status() == WL_CONNECTED) {
          WiFiClientSecure client;
          client.setInsecure();
          HTTPClient http;

          // Construct URL
          // Script expects GET format commonly.
          // Assuming Date/Time is handled by script or we send it.
          // Params: date, time, grid, solar, tuya, price

          char dateStr[20];
          char timeStr[20];
          strftime(dateStr, 20, "%d/%m/%Y", &timeinfo);
          strftime(timeStr, 20, "%H:%M:%S", &timeinfo);

          BenyData bd = getBenyData();

          String url = String(GOOGLE_SCRIPT_URL);
          url += "?date=" + String(dateStr);
          url += "&time=" + String(timeStr);
          url += "&grid=" + String((int)current_grid_power);
          url += "&solar=" + String((int)current_pv_power);
          url += "&price=" + String(getCurrentPrice(), 3);
          url += "&mode=" + String(charging_mode);
          url += "&beny_w=" + String((int)bd.power);
          url += "&paused=" + String(auto_paused ? 1 : 0);

          Serial.printf("GoogleSheets: Request: %s\n", url.c_str());

          http.begin(client, url);
          http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

          int httpCode = http.GET();
          if (httpCode > 0) {
            Serial.printf("GoogleSheets: Success, code: %d\n", httpCode);
          } else {
            Serial.printf("GoogleSheets: Failed, error: %s\n",
                          http.errorToString(httpCode).c_str());
          }
          http.end();

          sent_this_hour = true;
        }
      }
    } else if (timeinfo.tm_min != 0) {
      // Reset flag when minute is not 0
      sent_this_hour = false;
    }
  }
}
