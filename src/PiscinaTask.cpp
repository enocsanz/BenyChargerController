#include "PiscinaTask.h"
#include "BenyTask.h"
#include "EsiosTask.h"
#include "GoogleSheetsTask.h"
#include "HuaweiTask.h"
#include "TuyaLocal.h"
#include "config.h"
#include <Preferences.h>

// Solar surplus available for the pump, averaged (EMA) so that a passing cloud
// does not stop it. The pump has priority over the car: the surplus counts
// what the car is drawing, and the DLB then gives the car what is left.
static const float EMA_TAU = 300;              // s
static const unsigned long WARMUP = 60000;     // plain mean before the EMA
static const float START_MARGIN = 100;         // W over the pump's power to start
static const float STOP_FRACTION = 0.5;        // stop below this share of its power
static const unsigned long MIN_ON = 1800000;   // 30 min
static const unsigned long MIN_OFF = 900000;   // 15 min
static const int FILL_END_HOUR = 8;            // top-up window: 00:00 - 08:00
static const unsigned long SAVE_INTERVAL = 300000; // persist run time every 5 min
static const unsigned long GRID_STALE = 30000;
static const unsigned long CMD_RETRY = 10000;

int piscina_mode = PISCINA_AUTO;

static TuyaLocal relay;
static float maxHours[12] = PISCINA_MAX_HOURS;
static float minHours[12] = PISCINA_MIN_HOURS;
// Fixed, not learnt: the variable-speed motor changes its draw every so often
// (150-460 W measured), and the thresholds would move with it.
static const float pumpWatts = PISCINA_POWER;

static float surplusAvg = 0;
static bool haveAvg = false;
static bool solarRun = false;
static PiscinaReason reason = PR_OFFLINE;

// Daily accounting. runSecs: today's run time. deficitSecs: yesterday's
// shortfall against its minimum, topped up tonight before FILL_END_HOUR.
static int day = -1, dayMon = 0;
static float runSecs = 0, deficitSecs = 0;

static void save() {
  Preferences p;
  p.begin("beny", false);
  p.putInt("p_day", day);
  p.putInt("p_mon", dayMon);
  p.putInt("p_run", (int)runSecs);
  p.putInt("p_def", (int)deficitSecs);
  p.end();
}

void setupPiscina() {
  Preferences p;
  p.begin("beny", true);
  piscina_mode = p.getInt("p_mode", PISCINA_AUTO);
  day = p.getInt("p_day", -1);
  dayMon = p.getInt("p_mon", 0);
  runSecs = p.getInt("p_run", 0);
  deficitSecs = p.getInt("p_def", 0);
  char key[8];
  for (int m = 0; m < 12; m++) {
    snprintf(key, sizeof(key), "p_max%d", m);
    maxHours[m] = p.getFloat(key, maxHours[m]);
    snprintf(key, sizeof(key), "p_min%d", m);
    minHours[m] = p.getFloat(key, minHours[m]);
  }
  p.end();
  relay.begin("Piscina", PISCINA_IP, PISCINA_LOCAL_KEY, 34);
}

void setPiscinaMode(int mode) {
  piscina_mode = mode;
  Preferences p;
  p.begin("beny", false);
  p.putInt("p_mode", mode);
  p.end();
}

bool setPiscinaHours(float maxH, float minH) {
  struct tm t;
  if (!timeNow(&t) || maxH < 0 || maxH > 24 || minH < 0 || minH > maxH) return false;
  maxHours[t.tm_mon] = maxH;
  minHours[t.tm_mon] = minH;
  char key[8];
  Preferences p;
  p.begin("beny", false);
  snprintf(key, sizeof(key), "p_max%d", t.tm_mon);
  p.putFloat(key, maxH);
  snprintf(key, sizeof(key), "p_min%d", t.tm_mon);
  p.putFloat(key, minH);
  p.end();
  return true;
}

void loopPiscina() { relay.loop(); }

// Is `hour` among the `k` cheapest of the hours left in the top-up window?
// Without prices it tops up right away.
static bool isCheapHour(int hour, int k) {
  float p = getPriceAt(hour);
  if (p < 0) return true;
  int cheaper = 0;
  for (int h = hour + 1; h < FILL_END_HOUR; h++) {
    float q = getPriceAt(h);
    if (q < 0) return true;
    if (q < p) cheaper++;
  }
  return cheaper < k;
}

void runPiscinaLogic() {
  static unsigned long lastTick = 0;
  float dt = lastTick ? (millis() - lastTick) / 1000.0 : 0;
  lastTick = millis();

  if (!relay.connected() || relay.lastUpdate == 0) {
    reason = PR_OFFLINE;
    return;
  }

  // Run time, charged to the top-up or to today depending on why it runs
  if (relay.switchOn) {
    if (reason == PR_FILL) deficitSecs = max(0.0f, deficitSecs - dt);
    else runSecs += dt;
  }

  // Track how long the relay has been in its current state
  static bool lastOn = false;
  static unsigned long lastChange = 0;
  static bool seen = false;
  if (!seen || relay.switchOn != lastOn) {
    // At boot the relay's history is unknown: a running pump counts as just
    // started (it gets its MIN_ON, a reboot must not cycle it), a stopped one
    // may start right away.
    lastChange = (seen || relay.switchOn) ? millis() : millis() - MIN_OFF;
    lastOn = relay.switchOn;
    seen = true;
    save();
  }
  unsigned long inState = millis() - lastChange;

  // --- Day change: work out yesterday's shortfall ---
  struct tm t;
  bool haveTime = timeNow(&t);
  if (haveTime && t.tm_yday != day) {
    bool yesterday = (t.tm_yday == day + 1) || (t.tm_yday == 0 && day >= 364);
    deficitSecs = yesterday ? max(0.0f, minHours[dayMon] * 3600 - runSecs) : 0;
    if (deficitSecs > 0) {
      logEventf("PISCINA", "Ayer %.1f h de %.1f minimas, se completan %.1f h esta noche",
                runSecs / 3600, minHours[dayMon], deficitSecs / 3600);
    } else if (yesterday) {
      logEventf("PISCINA", "Ayer %.1f h (min %.1f, max %.1f)", runSecs / 3600, minHours[dayMon],
                maxHours[dayMon]);
    }
    day = t.tm_yday;
    dayMon = t.tm_mon;
    runSecs = 0;
    save();
  }
  float maxSecs = haveTime ? maxHours[t.tm_mon] * 3600 : 1e9;

  // --- Solar surplus (EMA), on every fresh grid sample ---
  // surplus = PV - house consumption without the car and the pump
  //         = car + pump - grid (capped at the PV production)
  // The first WARMUP of samples are a plain mean, and the EMA starts from it:
  // starting the EMA from the very first reading (often before the PV had been
  // read) stopped a pump that was running legitimately at boot.
  static uint32_t lastSample = 0;
  static unsigned long lastSampleTime = 0, firstSampleTime = 0;
  static float warmSum = 0;
  static int warmCount = 0;
  if (grid_sample_count != lastSample && pv_sample_count > 0) {
    float sampleDt = lastSampleTime ? (millis() - lastSampleTime) / 1000.0 : 0;
    lastSample = grid_sample_count;
    lastSampleTime = millis();
    BenyData bd = getBenyData();
    float pumpNow = relay.switchOn ? relay.power : 0;
    float s = min((float)current_pv_power, bd.power + pumpNow - current_grid_power);
    if (firstSampleTime == 0) firstSampleTime = millis();
    if (millis() - firstSampleTime < WARMUP) {
      warmSum += s;
      surplusAvg = warmSum / ++warmCount;
    } else {
      surplusAvg += (s - surplusAvg) * min(1.0f, sampleDt / EMA_TAU);
      haveAvg = true;
    }
  }
  bool gridFresh = lastSampleTime != 0 && millis() - lastSampleTime < GRID_STALE;

  // --- Top-up of yesterday's minimum, cheapest night hours ---
  bool fill = false;
  if (haveTime && deficitSecs > 0) {
    if (t.tm_hour < FILL_END_HOUR) {
      fill = isCheapHour(t.tm_hour, (int)ceilf(deficitSecs / 3600));
    } else {
      logEventf("PISCINA", "Quedan %.1f h sin completar, se descartan", deficitSecs / 3600);
      deficitSecs = 0;
      save();
    }
  }

  // --- Solar run, with hysteresis and minimum on/off times ---
  if (piscina_mode != PISCINA_AUTO) {
    solarRun = false;
  } else if (!solarRun) {
    if (haveAvg && gridFresh && runSecs < maxSecs && surplusAvg >= pumpWatts + START_MARGIN &&
        (!relay.switchOn ? inState >= MIN_OFF : true)) {
      solarRun = true;
      logEventf("PISCINA", "Arranque con sol (excedente medio %.0f W)", surplusAvg);
    }
  } else {
    bool maxReached = runSecs >= maxSecs;
    bool noSun = inState >= MIN_ON && surplusAvg < pumpWatts * STOP_FRACTION;
    if (maxReached || noSun) {
      solarRun = false;
      logEventf("PISCINA", "Parada (%s, excedente medio %.0f W, hoy %.1f h)",
                    maxReached ? "maximo diario" : "sin sol", surplusAvg, runSecs / 3600);
    }
  }

  bool desired;
  if (piscina_mode == PISCINA_ON) {
    desired = true;
    reason = PR_MANUAL_ON;
  } else if (piscina_mode == PISCINA_OFF) {
    desired = false;
    reason = PR_MANUAL_OFF;
  } else if (fill) {
    desired = true;
    reason = PR_FILL;
  } else if (solarRun) {
    desired = true;
    reason = PR_SOLAR;
  } else {
    desired = false;
    reason = runSecs >= maxSecs ? PR_DONE : PR_WAITING;
    // Right after boot, wait for the first surplus reading before stopping a
    // pump that may be running legitimately.
    if (!haveAvg) desired = relay.switchOn;
  }

  static unsigned long lastSave = 0;
  if (millis() - lastSave > SAVE_INTERVAL) {
    lastSave = millis();
    save();
  }

  // A new decision goes out at once; the same one is retried every CMD_RETRY
  static unsigned long lastCmd = 0;
  static bool lastSent = false;
  if (desired != relay.switchOn &&
      (lastCmd == 0 || desired != lastSent || millis() - lastCmd > CMD_RETRY)) {
    lastCmd = millis();
    lastSent = desired;
    logEventf("PISCINA", "Rele -> %s (excedente medio %.0f W, hoy %.1f h)",
                  desired ? "ON" : "OFF", surplusAvg, runSecs / 3600);
    relay.setSwitch(desired);
  }
}

PiscinaStatus getPiscinaStatus() {
  PiscinaStatus s;
  struct tm t;
  int mon = timeNow(&t) ? t.tm_mon : dayMon;
  s.online = relay.connected() && relay.lastUpdate != 0;
  s.relayOn = relay.switchOn;
  s.power = relay.power;
  s.reason = s.online ? reason : PR_OFFLINE;
  s.hoursToday = runSecs / 3600;
  s.maxHours = maxHours[mon];
  s.minHours = minHours[mon];
  s.deficitHours = deficitSecs / 3600;
  s.surplus = haveAvg ? surplusAvg : 0;
  return s;
}

String piscinaStatusText() {
  PiscinaStatus s = getPiscinaStatus();
  String modeStr = (piscina_mode == PISCINA_AUTO) ? "AUTO"
                   : (piscina_mode == PISCINA_ON) ? "ON"
                                                   : "OFF";
  String msg = "🏊 Depuradora: ";
  switch (s.reason) {
  case PR_OFFLINE:
    msg += "sin conexion con el rele";
    break;
  case PR_SOLAR:
    msg += "funcionando con sol (" + String(s.power, 0) + " W)";
    break;
  case PR_FILL:
    msg += "completando el minimo en hora barata (" + String(s.power, 0) + " W)";
    break;
  case PR_WAITING:
    msg += "parada, esperando sol";
    break;
  case PR_DONE:
    msg += "parada, maximo diario cumplido";
    break;
  case PR_MANUAL_ON:
    msg += "encendida manual";
    break;
  case PR_MANUAL_OFF:
    msg += "apagada manual";
    break;
  }
  msg += "\n   Hoy " + String(s.hoursToday, 1) + " h (max " + String(s.maxHours, 1) + ", min " +
         String(s.minHours, 1) + ")";
  if (s.deficitHours > 0) msg += " | Pendiente esta noche " + String(s.deficitHours, 1) + " h";
  msg += "\n   Excedente medio " + String(s.surplus, 0) + " W | Modo " + modeStr;
  return msg;
}
