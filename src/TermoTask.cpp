#include "TermoTask.h"
#include "BenyTask.h"
#include "EsiosTask.h"
#include "GoogleSheetsTask.h"
#include "HuaweiTask.h"
#include "TelegramTask.h"
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
  bool priceBlock = termo_mode == TERMO_AUTO && price >= 0 && price > termo_max_price;
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
    msg += "cortado por precio";
    break;
  case TR_OVERLOAD:
    msg += "cortado por sobrecarga, esperando margen";
    break;
  case TR_MANUAL:
    msg += "apagado manual";
    break;
  }
  msg += "\n   Modo " + modeStr + " | Umbral " + String(termo_max_price, 3) + " E/kWh";
  return msg;
}
