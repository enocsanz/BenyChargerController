#include "GoogleSheetsTask.h"
#include "BenyTask.h"
#include "EsiosTask.h"
#include "HuaweiTask.h"
#include "PiscinaTask.h"
#include "TermoTask.h"
#include "config.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

// Externs for data to log
extern float getCurrentPrice();
extern int charging_mode;
extern int target_amps;

unsigned long lastSheetsCheck = 0;
bool sent_this_hour = false;

// --- Diagnostic log ---
// One sample per minute and the events, POSTed as JSON every SEND_INTERVAL
// (one TLS request per batch instead of one per minute: each request stalls
// the loop for a couple of seconds). A failed batch is kept and retried; past
// MAX_SAMPLES / MAX_EVENTS the oldest entries are dropped.
static const unsigned long SAMPLE_INTERVAL = 60000;
static const unsigned long SEND_INTERVAL = 300000;
static const int MAX_SAMPLES = 30;
static const int MAX_EVENTS = 40;

bool diag_enabled = true;
static String samples[MAX_SAMPLES];
static int sampleCount = 0;
static String events[MAX_EVENTS];
static int eventCount = 0;
static int dropped = 0;
static String lastResult = "sin envios aun";

static int32_t gridMin = INT32_MAX, gridMax = INT32_MIN;

static const char *TERMO_NAMES[] = {"HABILITADO", "PRECIO", "SOBRECARGA", "MANUAL",
                                    "SIN CONEXION"};
static const char *PISCINA_NAMES[] = {"SOL",       "MINIMO",     "ESPERA",      "MAX CUMPLIDO",
                                      "MANUAL ON", "MANUAL OFF", "SIN CONEXION"};

static String jsonStr(const String &s) {
  String out = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') out += "\\n";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out + "\"";
}

// "dd/mm/yyyy","HH:MM:SS" (or the uptime, before NTP)
static String stamp() {
  struct tm t;
  char buf[32];
  if (timeNow(&t)) strftime(buf, sizeof(buf), "\"%d/%m/%Y\",\"%H:%M:%S\"", &t);
  else snprintf(buf, sizeof(buf), "\"sin hora\",\"+%lus\"", millis() / 1000);
  return buf;
}

void logEvent(const char *type, const String &detail) {
  Serial.printf("EVENTO %s: %s\n", type, detail.c_str());
  if (!diag_enabled) return;
  if (eventCount == MAX_EVENTS) {
    for (int i = 1; i < MAX_EVENTS; i++) events[i - 1] = events[i];
    eventCount--;
    dropped++;
  }
  events[eventCount++] = "[" + stamp() + "," + jsonStr(type) + "," + jsonStr(detail) + "]";
}

void logEventf(const char *type, const char *fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logEvent(type, buf);
}

void setDiagEnabled(bool on) {
  diag_enabled = on;
  Preferences p;
  p.begin("beny", false);
  p.putBool("diag", on);
  p.end();
}

String diagStatusText() {
  String msg = "📈 Diagnostico Google Sheets: ";
  msg += diag_enabled ? "ACTIVO" : "desactivado";
  msg += "\n   Pendiente: " + String(sampleCount) + " muestras, " + String(eventCount) + " eventos";
  if (dropped) msg += " (" + String(dropped) + " descartados)";
  msg += "\n   Ultimo envio: " + lastResult;
  return msg;
}

static void takeSample() {
  BenyData bd = getBenyData();
  TermoStatus ts = getTermoStatus();
  PiscinaStatus ps = getPiscinaStatus();
  if (gridMin == INT32_MAX) gridMin = gridMax = current_grid_power;

  String s = "[" + stamp();
  s += "," + String((int)current_grid_power) + "," + String((int)gridMin) + "," +
       String((int)gridMax);
  s += "," + String((int)current_pv_power) + "," + String(getCurrentPrice(), 3);
  s += "," + String(charging_mode) + "," + String((int)bd.power) + "," + jsonStr(bd.status);
  s += "," + String(target_amps) + "," + String(bd.current, 1);
  s += "," + jsonStr(TERMO_NAMES[ts.reason]) + "," + String((int)ts.power) + "," +
       String(ts.relayOn ? 1 : 0);
  s += "," + jsonStr(PISCINA_NAMES[ps.reason]) + "," + String((int)ps.power) + "," +
       String(ps.relayOn ? 1 : 0) + "," + String(ps.hoursToday, 2) + "," +
       String((int)ps.surplus);
  s += "," + String(ESP.getFreeHeap()) + "," + String(WiFi.RSSI()) + "," +
       String(millis() / 60000);
  // Chip temperature, to keep an eye on it once the board is enclosed
  s += "," + String(temperatureRead(), 1);
  s += "]";

  if (sampleCount == MAX_SAMPLES) {
    for (int i = 1; i < MAX_SAMPLES; i++) samples[i - 1] = samples[i];
    sampleCount--;
    dropped++;
  }
  samples[sampleCount++] = s;
  gridMin = INT32_MAX;
  gridMax = INT32_MIN;
}

static void sendBatch() {
  String body = "{\"samples\":[";
  for (int i = 0; i < sampleCount; i++) body += (i ? "," : "") + samples[i];
  body += "],\"events\":[";
  for (int i = 0; i < eventCount; i++) body += (i ? "," : "") + events[i];
  body += "]}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/json");
  // Apps Script runs doPost() and then answers 302 to fetch the result:
  // that 302 already means the rows were written, so it is not followed.
  // A 200 is Google's own error page (e.g. an old deployment without doPost),
  // so it is a failure and the batch is kept.
  // Timed: the TLS handshake is the first thing a lower CPU clock would slow
  unsigned long t0 = millis();
  int code = http.POST(body);
  http.end();
  float secs = (millis() - t0) / 1000.0;

  char when[12];
  struct tm t;
  if (timeNow(&t)) strftime(when, sizeof(when), "%H:%M", &t);
  else strcpy(when, "?");

  if (code == 302) {
    Serial.printf("GoogleSheets: Diagnostico enviado (%d muestras, %d eventos, %u bytes, %.1f s)\n",
                  sampleCount, eventCount, body.length(), secs);
    lastResult = String(when) + " OK (" + String(sampleCount) + " muestras, " +
                 String(eventCount) + " eventos, " + String(secs, 1) + " s)";
    sampleCount = 0;
    eventCount = 0;
  } else {
    Serial.printf("GoogleSheets: Diagnostico fallo (%d), se reintenta\n", code);
    lastResult = String(when) + " FALLO " + String(code) + " (" + String(secs, 1) +
                 " s), se reintenta";
  }
}

static void loopDiag() {
  static unsigned long lastSend = 0;
  bool sendDue = millis() - lastSend >= SEND_INTERVAL && WiFi.status() == WL_CONNECTED;

  if (!diag_enabled) {
    // Still an (empty) batch now and then, as a heartbeat: the script's
    // watchdog alerts on Telegram when nothing arrives for a while.
    if (sendDue) {
      lastSend = millis();
      sendBatch();
    }
    return;
  }

  // Grid extremes within the minute, from every fresh reading
  static uint32_t lastGridSample = 0;
  if (grid_sample_count != lastGridSample) {
    lastGridSample = grid_sample_count;
    gridMin = min(gridMin, current_grid_power);
    gridMax = max(gridMax, current_grid_power);
  }

  // Charger state changes as events
  static String lastBenyStatus = "";
  BenyData bd = getBenyData();
  if (bd.status != lastBenyStatus) {
    if (lastBenyStatus != "") logEvent("BENY", lastBenyStatus + " -> " + bd.status);
    lastBenyStatus = bd.status;
  }

  static unsigned long lastSample = 0;
  if (millis() - lastSample >= SAMPLE_INTERVAL) {
    lastSample = millis();
    takeSample();
  }

  if (sendDue) {
    lastSend = millis();
    sendBatch();
  }
}

void setupGoogleSheets() {
  Preferences p;
  p.begin("beny", true);
  diag_enabled = p.getBool("diag", true);
  p.end();
  Serial.printf("GoogleSheets: Initialized (diagnostico %s).\n", diag_enabled ? "ON" : "OFF");
}

void loopGoogleSheets() {
  loopDiag();

  // Check every 10 seconds to catch the minute 0
  if (millis() - lastSheetsCheck > 10000) {
    lastSheetsCheck = millis();

    struct tm timeinfo;
    if (!timeNow(&timeinfo))
      return;

    // Condition: Hourly logging (Minute == 0)
    if (timeinfo.tm_min == 0) {
      if (!sent_this_hour) {
        Serial.println("GoogleSheets: Sending hourly log...");

        if (WiFi.status() == WL_CONNECTED) {
          WiFiClientSecure client;
          client.setInsecure();
          HTTPClient http;

          // Construct URL. Params: date, time, grid, solar, price, mode,
          // beny_w, amps (see google_apps_script.js)
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
          url += "&amps=" + String(target_amps);

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
