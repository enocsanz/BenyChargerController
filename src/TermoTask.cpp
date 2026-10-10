#include "TermoTask.h"
#include "BenyTask.h"
#include "EsiosTask.h"
#include "GoogleSheetsTask.h"
#include "HuaweiTask.h"
#include "TelegramTask.h"
#include "SondaLink.h"
#include "TuyaLocal.h"
#include "config.h"
#include <Preferences.h>

extern int target_amps; // DLB setpoint, from main.cpp

// Over the contracted power by more than this, for SHED_DELAY, with the car
// already at its floor -> cut the heater. Short peaks are tolerated by the
// distributor, so they are ignored on purpose.
static const int32_t SHED_MARGIN = 200;               // W
static const unsigned long SHED_DELAY = 30000;        // 30 s
// Back on only after MIN_OFF_TIME, and once the heater fits for RESTORE_DELAY
static const unsigned long MIN_OFF_TIME = 300000;     // 5 min
static const unsigned long RESTORE_DELAY = 120000;    // 2 min
// Grid reading older than this is not acted upon
static const unsigned long GRID_STALE = 30000;
// Without any grid reading for this long, switch on without the room check
static const unsigned long GRID_WAIT = 300000;        // 5 min
// Relay's own countdown while cut: back on by itself if the controller stops
static const uint32_t FAILSAFE_SECS = 900;            // 15 min
// Last known price kept through a gap this long
static const unsigned long PRICE_HOLD = 600000;       // 10 min
// Retry a relay command that did not take
static const unsigned long CMD_RETRY = 10000;

int termo_mode = TERMO_AUTO;
float termo_max_price = TERMO_MAX_PRICE;

static TuyaLocal relay;
static bool overloadCut = false;
static bool cutNotified = false; // a real overload cut, announced on Telegram
static unsigned long cutTime = 0;
static float heaterWatts = TERMO_DEFAULT_POWER; // learnt from the relay
static TermoReason reason = TR_OFFLINE;

void setupTermo() {
  Preferences p;
  p.begin("beny", true);
  termo_mode = p.getInt("t_mode", TERMO_AUTO);
  termo_max_price = p.getFloat("t_price", TERMO_MAX_PRICE);
  p.end();
  relay.begin("Termo", TERMO_IP, TERMO_LOCAL_KEY, 35);
}

void setTermoMode(int mode) {
  termo_mode = mode;
  Preferences p;
  p.begin("beny", false);
  p.putInt("t_mode", mode);
  p.end();
}

void setTermoMaxPrice(float price) {
  termo_max_price = price;
  Preferences p;
  p.begin("beny", false);
  p.putFloat("t_price", price);
  p.end();
}

void loopTermo() { relay.loop(); }

// --- Smart control with the water probe (AUTO mode, fresh probe reading) ---
// Measured over 44 h: the tank barely loses heat (0.1-0.2 C/h) and is full
// after every cycle; what made it expensive was WHEN it recharged: right after
// each use, even at 0.36 EUR/kWh. So after a use it waits for a cheap hour,
// unless the water gets too low. Temperatures are on the probe's scale (it
// sits at the mouth of the thermostat's well, below the real water
// temperature): full ~38-39 C, the thermostat calls for heat at ~32.5 C.
static const float SMART_CHEAP_ABS = 0.15;  // EUR/kWh: always cheap enough
static const int SMART_CHEAP_HOURS = 8;     // the day's cheapest hours
static const float SMART_MIN = 28.0;        // comfort floor: heat even if expensive...
static const float SMART_MIN_STOP = 31.0;   // ...but only up to here
// During a shower the probe dips sharply (cold water comes in at the bottom,
// where the well is) and recovers several degrees by itself within ~20 min
// once the tank mixes (seen: 24.4 -> 31.1 C). The floor only counts if the
// water stays below it this long, or every shower would recharge at peak price.
static const unsigned long SMART_MIN_HOLD = 1200000; // 20 min
static const float SMART_MORNING = 34.0;    // tank not full before the morning shower
static const int SMART_MORNING_END = 6;     // ...ready by 06:30 (shower ~06:45)
static String smartNote = "";               // why, for /termo and the log

// How many of today's known hours are cheaper than `hour`; -1 if unknown
static int cheaperHours(int hour) {
  float p = getPriceAt(hour);
  if (p < 0) return -1;
  int n = 0;
  for (int h = 0; h < 24; h++) {
    float q = getPriceAt(h);
    if (q >= 0 && q < p) n++;
  }
  return n;
}

// Next hour from now (today) among the cheapest ones, for the message
static String nextCheapHour(int from) {
  for (int h = from + 1; h < 24; h++) {
    int r = cheaperHours(h);
    float p = getPriceAt(h);
    if ((p >= 0 && p <= SMART_CHEAP_ABS) || (r >= 0 && r < SMART_CHEAP_HOURS))
      return String(h) + "h a " + String(p, 3);
  }
  return "manana";
}

// True when the heater may heat now. Sets smartNote.
static bool smartAllows(float water, float price) {
  static bool lowWater = false;
  static unsigned long belowSince = 0;
  if (water < SMART_MIN) {
    if (belowSince == 0) belowSince = millis();
    if (millis() - belowSince >= SMART_MIN_HOLD) lowWater = true;
  } else {
    belowSince = 0;
    if (water >= SMART_MIN_STOP) lowWater = false;
  }
  if (lowWater) {
    smartNote = "minimo de confort (agua por debajo de " + String(SMART_MIN, 0) + " C mas de 20 min)";
    return true;
  }

  struct tm t;
  if (!timeNow(&t) || price < 0) {
    smartNote = "sin hora o sin precio: no se bloquea";
    return true;
  }
  int h = t.tm_hour;
  if (price <= SMART_CHEAP_ABS) {
    smartNote = "precio bajo (" + String(price, 3) + ")";
    return true;
  }
  int rank = cheaperHours(h);
  if (rank < 0 || rank < SMART_CHEAP_HOURS) {
    smartNote = "entre las " + String(SMART_CHEAP_HOURS) + " horas mas baratas del dia";
    return true;
  }

  // Morning: tank not full -> heat in the cheapest hour left before 06:30,
  // and from 06:00 whatever the price
  bool morning = h < SMART_MORNING_END || (h == SMART_MORNING_END && t.tm_min < 30);
  if (morning && water < SMART_MORNING) {
    bool cheapest = true;
    for (int k = h + 1; k <= SMART_MORNING_END; k++) {
      float q = getPriceAt(k);
      if (q >= 0 && q < price) cheapest = false;
    }
    if (cheapest || h >= SMART_MORNING_END) {
      smartNote = "preparando la ducha de la manana";
      return true;
    }
  }

  smartNote = "esperando hora barata (proxima: " + nextCheapHour(h) + ")";
  return false;
}

// Heating cycles, as events for the analysis of when the heater runs: start
// above CYCLE_ON_W, end below CYCLE_OFF_W (hysteresis against noise in the
// reading). Energy and cost are integrated every call (1 s), with the price
// of each moment, so a cycle across an hour change gets its real average.
static const float CYCLE_ON_W = 500;
static const float CYCLE_OFF_W = 100;

static void trackHeatingCycle() {
  static bool heating = false;
  static unsigned long startedAt = 0, lastTick = 0;
  static double wh = 0, cost = 0, pricedWh = 0;

  unsigned long now = millis();
  float dt = lastTick ? (now - lastTick) / 1000.0 : 0;
  lastTick = now;
  float w = relay.switchOn ? relay.power : 0;

  if (!heating && w > CYCLE_ON_W) {
    heating = true;
    startedAt = now;
    wh = cost = pricedWh = 0;
    logEventf("TERMO", "Empieza a calentar (%.0f W, precio %.3f E/kWh, red %d W)", w,
              getCurrentPrice(), current_grid_power);
    return;
  }
  if (!heating) return;

  double stepWh = w * dt / 3600.0;
  wh += stepWh;
  float price = getCurrentPrice();
  if (price >= 0) {
    cost += stepWh / 1000.0 * price;
    pricedWh += stepWh;
  }

  if (w < CYCLE_OFF_W) {
    heating = false;
    float mins = (now - startedAt) / 60000.0;
    if (pricedWh > 0) {
      logEventf("TERMO", "Deja de calentar: %.0f min, %.2f kWh, precio medio %.3f E/kWh, %.2f EUR",
                mins, wh / 1000.0, cost * 1000.0 / pricedWh, cost);
    } else {
      logEventf("TERMO", "Deja de calentar: %.0f min, %.2f kWh (sin precio)", mins, wh / 1000.0);
    }
  }
}

// Overload the system can no longer fix: car already at its floor (or not
// charging) and the heater not drawing. Nothing left to cut from here, so the
// user is told to switch something off before the distributor cuts. Works on
// the grid's 1-minute average, so a single dip does not restart the count
// and a single peak does not trigger it.
static const unsigned long ALERT_DELAY = 180000;   // 3 min sustained
static const unsigned long ALERT_CLEAR = 120000;   // 2 min below the limit
static const float ALERT_TAU = 60;                 // s, grid average

static void checkOverloadAlert() {
  static uint32_t lastSample = 0;
  static unsigned long lastSampleTime = 0;
  static float gridAvg = 0;
  if (grid_sample_count == lastSample) {
    return; // only on fresh readings: a stale one says nothing
  }
  float dt = lastSampleTime ? (millis() - lastSampleTime) / 1000.0 : ALERT_TAU;
  lastSample = grid_sample_count;
  lastSampleTime = millis();
  gridAvg += (current_grid_power - gridAvg) * min(1.0f, dt / ALERT_TAU);

  BenyData bd = getBenyData();
  bool carCharging = (bd.status == "CHARGING" || bd.status == "STARTING");
  bool carAtFloor = !carCharging || target_amps <= BENY_MIN_AMPS;
  bool heaterOff = !relay.connected() || !relay.switchOn || relay.power < 100;
  bool stuck = carAtFloor && heaterOff && gridAvg > CONTRACTED_POWER + SHED_MARGIN;

  static unsigned long stuckSince = 0, okSince = 0;
  static bool alerted = false;
  if (!stuck) stuckSince = 0;
  else if (stuckSince == 0) stuckSince = millis();

  if (!alerted && stuckSince != 0 && millis() - stuckSince > ALERT_DELAY) {
    alerted = true;
    okSince = 0;
    String car = carCharging ? "coche al minimo (" + String(BENY_MIN_AMPS) + "A)" : "sin coche cargando";
    logEventf("SOBRECARGA", "Sostenida: media %.0f W, %s, termo sin consumo", gridAvg, car.c_str());
    sendTelegramNotification("🚨 Sobrecarga sostenida: " + String(gridAvg, 0) + " W de media (limite " +
                             String(CONTRACTED_POWER) + " W), " + car +
                             " y termo sin consumo. Ya no queda nada que cortar desde aqui: "
                             "apaga algo" +
                             String(carCharging ? " o para el coche" : "") + ".");
  }

  if (alerted) {
    if (gridAvg >= CONTRACTED_POWER) okSince = 0;
    else if (okSince == 0) okSince = millis();
    if (okSince != 0 && millis() - okSince > ALERT_CLEAR) {
      alerted = false;
      logEventf("SOBRECARGA", "Resuelta: media %.0f W", gridAvg);
      sendTelegramNotification("✅ Sobrecarga resuelta: " + String(gridAvg, 0) + " W de media.");
    }
  }
}

void runTermoLogic() {
  checkOverloadAlert();

  if (!relay.connected() || relay.lastUpdate == 0) {
    reason = TR_OFFLINE;
    return;
  }

  // Learn the heater's real power whenever it is heating
  if (relay.switchOn && relay.power > 500) heaterWatts = relay.power;

  trackHeatingCycle();

  // Only act on a live grid reading
  static uint32_t lastSample = 0;
  static unsigned long lastSampleTime = 0;
  if (grid_sample_count != lastSample) {
    lastSample = grid_sample_count;
    lastSampleTime = millis();
  }
  bool gridFresh = lastSampleTime != 0 && millis() - lastSampleTime < GRID_STALE;

  // The car yields first: the DLB lowers it to BENY_MIN_AMPS before the heater
  // is touched. carSpare is what it could still give back.
  BenyData bd = getBenyData();
  bool carCharging = (bd.status == "CHARGING" || bd.status == "STARTING");
  bool carAtFloor = !carCharging || target_amps <= BENY_MIN_AMPS;
  float volts = (bd.voltage > 100) ? bd.voltage : 230.0;
  float carSpare = carCharging ? max(0.0f, bd.current - BENY_MIN_AMPS) * volts : 0;

  // Would the heater fit with the car at its floor? (What it draws now is
  // already in the grid reading, so it is taken out first.)
  float heaterNow = relay.switchOn ? relay.power : 0;
  bool hasRoom = gridFresh && current_grid_power - carSpare - heaterNow + heaterWatts <=
                                  CONTRACTED_POWER - SHED_MARGIN;

  // --- Overload: cut after SHED_DELAY sustained ---
  static unsigned long overSince = 0;
  bool over = gridFresh && carAtFloor && current_grid_power > CONTRACTED_POWER + SHED_MARGIN;
  if (!over) overSince = 0;
  else if (overSince == 0) overSince = millis();

  if (!overloadCut && relay.switchOn && overSince != 0 && millis() - overSince > SHED_DELAY) {
    overloadCut = true;
    cutNotified = true;
    cutTime = millis();
    logEventf("TERMO", "Corte por sobrecarga (red %d W)", current_grid_power);
    sendTelegramNotification("⚠️ Termo cortado por sobrecarga: red " +
                             String(current_grid_power) + " W con el coche al minimo. "
                             "Vuelve cuando haya margen.");
  }

  // --- Back on after MIN_OFF_TIME, once there is room for RESTORE_DELAY ---
  static unsigned long roomSince = 0;
  if (!hasRoom) roomSince = 0;
  else if (roomSince == 0) roomSince = millis();

  if (overloadCut && millis() - cutTime > MIN_OFF_TIME && roomSince != 0 &&
      millis() - roomSince > RESTORE_DELAY) {
    overloadCut = false;
    logEvent("TERMO", "Hay margen, se reactiva");
    if (cutNotified) sendTelegramNotification("✅ Termo reactivado: vuelve a haber margen en la red.");
    cutNotified = false;
  }

  // --- Desired state ---
  // -1 if unknown: then the price never blocks. A brief gap keeps the last
  // known price for PRICE_HOLD, so only a real ESIOS outage lets it through:
  // a one-second "unknown" used to switch the heater on and off again.
  float price = getCurrentPrice();
  static float lastPrice = -1;
  static unsigned long lastPriceAt = 0;
  if (price >= 0) {
    lastPrice = price;
    lastPriceAt = millis();
  } else if (lastPriceAt != 0 && millis() - lastPriceAt < PRICE_HOLD) {
    price = lastPrice;
  }
  // With a fresh probe reading AUTO uses the smart control; without it, the
  // plain price threshold
  float water = sondaWaterTemp();
  bool priceBlock;
  if (termo_mode == TERMO_AUTO && !isnan(water)) {
    String before = smartNote;
    priceBlock = !smartAllows(water, price);
    if (smartNote.substring(0, 12) != before.substring(0, 12)) {
      logEventf("TERMO", "Inteligente: %s (agua %.1f C, precio %.3f)", smartNote.c_str(), water,
                price);
    }
  } else {
    smartNote = "";
    priceBlock = termo_mode == TERMO_AUTO && price >= 0 && price > termo_max_price;
  }
  bool allowed = termo_mode != TERMO_OFF && !priceBlock;

  // Being allowed again (price drops, back to AUTO/ON, boot) also needs room:
  // otherwise the heater would start right into an overload. Wait for room,
  // without MIN_OFF_TIME. Only on that edge, so grid noise cannot bounce it,
  // and only with a grid reading: at boot, before the first one, "no reading"
  // was taken for "no room" and flagged a false overload.
  static bool wasAllowed = false;
  if (gridFresh) {
    bool becameAllowed = allowed && !wasAllowed;
    wasAllowed = allowed;
    if (becameAllowed && !relay.switchOn && !overloadCut && !hasRoom) {
      overloadCut = true;
      cutTime = millis() - MIN_OFF_TIME;
      logEvent("TERMO", "Sin margen para encender, esperando");
    }
  }

  bool desired = allowed && !overloadCut;

  // No grid reading yet: do not switch on blind, but only for GRID_WAIT. If
  // the inverter stays silent, the heater must not stay off because of it.
  unsigned long gridAge = lastSampleTime ? millis() - lastSampleTime : millis();
  if (desired && !relay.switchOn && !gridFresh && gridAge < GRID_WAIT) desired = false;

  TermoReason prev = reason;
  if (termo_mode == TERMO_OFF) reason = TR_MANUAL;
  else if (overloadCut) reason = TR_OVERLOAD;
  else if (priceBlock) reason = TR_PRICE;
  else reason = TR_ENABLED;
  if (reason != prev) {
    static const char *names[] = {"HABILITADO", "PRECIO", "SOBRECARGA", "MANUAL", "SIN CONEXION"};
    logEventf("TERMO", "Estado %s (precio %.3f, umbral %.3f, red %d W)", names[reason], price,
                  termo_max_price, current_grid_power);
  }

  // A new decision goes out at once; the same one is only retried every
  // CMD_RETRY, in case the relay did not take it.
  static unsigned long lastCmd = 0;
  static bool lastSent = false;
  if (desired != relay.switchOn &&
      (lastCmd == 0 || desired != lastSent || millis() - lastCmd > CMD_RETRY)) {
    lastCmd = millis();
    lastSent = desired;
    logEventf("TERMO", "Rele -> %s (precio %.3f, red %d W)", desired ? "ON" : "OFF", price,
                  current_grid_power);
    relay.setSwitch(desired);
  } else if (desired == relay.switchOn) {
    // While cut (price or overload) the relay counts down FAILSAFE_SECS and
    // switches back on by itself if this controller stops: a dead controller
    // must not leave the house without hot water. Not with TERMO_OFF, which
    // is the user's own choice.
    relay.keepFailsafe(!desired && termo_mode != TERMO_OFF ? FAILSAFE_SECS : 0);
  }
}

TermoStatus getTermoStatus() {
  TermoStatus s;
  s.online = relay.connected() && relay.lastUpdate != 0;
  s.relayOn = relay.switchOn;
  s.power = relay.power;
  s.reason = s.online ? reason : TR_OFFLINE;
  return s;
}

String termoStatusText() {
  TermoStatus s = getTermoStatus();
  String modeStr = (termo_mode == TERMO_AUTO) ? "AUTO" : (termo_mode == TERMO_ON) ? "ON" : "OFF";
  String msg = "🚿 Termo (ACS): ";
  switch (s.reason) {
  case TR_OFFLINE:
    msg += "sin conexion con el rele";
    break;
  case TR_ENABLED:
    msg += s.power > 100 ? "calentando " + String(s.power, 0) + " W" : "encendido (termostato en reposo)";
    break;
  case TR_PRICE:
    msg += smartNote.length() ? "cortado, " + smartNote : String("cortado por precio");
    break;
  case TR_OVERLOAD:
    msg += "cortado por sobrecarga, esperando margen";
    break;
  case TR_MANUAL:
    msg += "apagado manual";
    break;
  }
  if (smartNote.length()) {
    msg += "\n   Control inteligente: " + smartNote;
    msg += "\n   Modo " + modeStr + " | Calienta: <= " + String(SMART_CHEAP_ABS, 2) + " E/kWh, " +
           String(SMART_CHEAP_HOURS) + " horas mas baratas, antes de las 06:30 si < " +
           String(SMART_MORNING, 0) + " C, o si el agua < " + String(SMART_MIN, 0) + " C (hasta " +
           String(SMART_MIN_STOP, 0) + " C)";
  } else {
    msg += "\n   Modo " + modeStr + " | Umbral " + String(termo_max_price, 3) + " E/kWh" +
           (termo_mode == TERMO_AUTO ? " (sin sonda: control por umbral)" : "");
  }
  float agua = sondaWaterTemp(), agua2 = sondaWaterTemp2();
  msg += isnan(agua) ? String("\n   Agua: sin sonda conectada")
                     : "\n   Agua abajo: " + String(agua, 1) + " C (vaina del termostato)";
  if (!isnan(agua2)) msg += "\n   Agua a media altura: " + String(agua2, 1) + " C (serpentin)";
  return msg;
}
