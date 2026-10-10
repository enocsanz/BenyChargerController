#include "SondaLink.h"
#include "GoogleSheetsTask.h"
#include "SondaPacket.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>

// The probe sends one message per second, numbered, either over the home WiFi
// (UDP, port SONDA_UDP_PORT) or directly over ESP-NOW. Over UDP each message
// is echoed back, so the probe can show how many got through. Delivery here is
// measured from the sequence numbers: received / (last seq - first seq + 1).
// The ESP-NOW callback runs in the WiFi task, so it only updates counters.
static volatile uint32_t rxCount = 0;  // messages received since the probe's boot
static volatile uint32_t firstSeq = 0; // first seq seen since the probe's boot
static volatile uint32_t lastSeq = 0;
static volatile unsigned long lastRxAt = 0;
static volatile float lastTemp = NAN;
static volatile uint32_t probeUptime = 0;
static volatile bool lastViaUdp = false;
static volatile int8_t probeRssi = 0;
static char probeFw[22] = "";
static volatile uint8_t probeSensors = 0;
static volatile float probeRawTemp = NAN;
static volatile uint8_t probeLine = 0;
static volatile float lastTemp2 = NAN; // mid height (coil)
static IPAddress probeIp;
// A reading older than this is not used (nor shown as current)
static const unsigned long SONDA_STALE = 300000; // 5 min
static bool started = false;
static WiFiUDP udp;

// Last-minute and last-hour snapshots
static uint32_t minRx = 0, minSeq = 0, hourRx = 0, hourSeq = 0;
static float lastMinutePct = -1;

static bool handlePacket(const uint8_t *data, int len, bool viaUdp) {
  if (len != sizeof(SondaPacket)) return false;
  SondaPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != SONDA_MAGIC) return false;
  if (p.seq < lastSeq || rxCount == 0) { // probe rebooted: start counting again
    rxCount = 0;
    firstSeq = p.seq;
    minRx = hourRx = 0;
    minSeq = hourSeq = p.seq - 1;
  }
  if (p.seq != lastSeq || rxCount == 0) rxCount = rxCount + 1; // ignore repeats
  lastSeq = p.seq;
  lastRxAt = millis();
  lastTemp = p.temp;
  probeUptime = p.uptime;
  lastViaUdp = viaUdp;
  probeRssi = p.rssi;
  memcpy(probeFw, p.fw, sizeof(p.fw));
  probeSensors = p.sensors;
  probeRawTemp = p.rawTemp;
  probeLine = p.line;
  lastTemp2 = p.temp2;
  probeFw[sizeof(p.fw)] = 0;
  return true;
}

static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  handlePacket(data, len, false);
}

void setupSondaLink() {
  if (started) return;
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onRecv);
  else logEvent("SONDA", "ESP-NOW no se pudo iniciar");
  udp.begin(SONDA_UDP_PORT);
  started = true;
  Serial.printf("Sonda: escuchando UDP %u y ESP-NOW (canal %d), MAC %s\n", SONDA_UDP_PORT,
                WiFi.channel(), WiFi.macAddress().c_str());
}

static float pct(uint32_t rx, uint32_t seqSpan) {
  return seqSpan ? 100.0 * rx / seqSpan : -1;
}

void loopSondaLink() {
  if (!started) return;

  // UDP: handle what arrived and echo it back as the confirmation
  for (int n = udp.parsePacket(); n > 0; n = udp.parsePacket()) {
    uint8_t buf[64];
    int len = udp.read(buf, sizeof(buf));
    if (handlePacket(buf, len, true)) {
      probeIp = udp.remoteIP();
      udp.beginPacket(udp.remoteIP(), udp.remotePort());
      udp.write(buf, len);
      udp.endPacket();
    }
  }

  static unsigned long lastMinute = 0, lastHour = 0;
  if (millis() - lastMinute >= 60000) {
    lastMinute = millis();
    uint32_t rx = rxCount, seq = lastSeq;
    lastMinutePct = (seq > minSeq) ? pct(rx - minRx, seq - minSeq) : -1;
    minRx = rx;
    minSeq = seq;
  }
  if (lastHour == 0) lastHour = millis();
  if (millis() - lastHour >= 3600000) {
    lastHour = millis();
    uint32_t rx = rxCount, seq = lastSeq;
    if (seq > hourSeq) {
      logEventf("SONDA", "Ultima hora: %.0f %% recibido (%u de %u, por %s)",
                pct(rx - hourRx, seq - hourSeq), rx - hourRx, seq - hourSeq,
                lastViaUdp ? "WiFi" : "ESP-NOW");
    } else {
      logEvent("SONDA", "Ultima hora: ningun mensaje de la sonda");
    }
    hourRx = rx;
    hourSeq = seq;
  }
}

static const char *rssiWord(int rssi) {
  return rssi >= -67 ? "buena" : rssi >= -75 ? "aceptable" : rssi >= -80 ? "justa" : "mala";
}

static bool fresh() { return rxCount > 0 && millis() - lastRxAt < SONDA_STALE; }

float sondaWaterTemp() { return fresh() ? lastTemp : NAN; }
float sondaWaterTemp2() { return fresh() ? lastTemp2 : NAN; }

String sondaShortText() {
  if (!started || rxCount == 0) return "📡 Sonda Termo: ningun mensaje recibido";
  if (!fresh())
    return "📡 Sonda Termo: sin senal desde hace " + String((millis() - lastRxAt) / 60000) + " min";
  String msg = "📡 Sonda Termo: ";
  msg += lastMinutePct >= 0 ? String(lastMinutePct, 0) + " % ultimo minuto" : "conectada";
  if (probeRssi != 0) msg += " | WiFi " + String(probeRssi) + " dBm (" + rssiWord(probeRssi) + ")";
  if (!isnan(lastTemp)) msg += " | Agua abajo " + String(lastTemp, 1) + " C";
  if (!isnan(lastTemp2)) msg += ", medio " + String(lastTemp2, 1) + " C";
  return msg;
}

String sondaVersionText() {
  if (rxCount == 0 || probeFw[0] == 0) return "📡 Sonda Termo: sin datos de version todavia";
  String msg = "📡 Sonda Termo: firmware " + String(probeFw);
  if (probeIp) msg += "\n   IP " + probeIp.toString();
  msg += "\n   Actualizar: cd sonda && pio run -e m5stickcplus_ota -t upload";
  return msg;
}

String sondaWifiText() {
  if (!fresh()) return "📡 Sonda Termo: sin mensajes recientes";
  if (probeRssi == 0) return "📡 Sonda Termo: por ESP-NOW (sin dato de WiFi)";
  return "📡 Sonda Termo: " + String(probeRssi) + " dBm (" + rssiWord(probeRssi) + ")";
}

String sondaLinkText() {
  if (!started) return "📡 Sonda Termo: enlace no iniciado";
  String msg = "📡 Sonda Termo\n";
  if (rxCount == 0) {
    return msg + "   Ningun mensaje recibido todavia\n   Este controlador: IP " +
           WiFi.localIP().toString() + ", UDP " + String(SONDA_UDP_PORT) + ", MAC " +
           WiFi.macAddress();
  }
  unsigned long ago = (millis() - lastRxAt) / 1000;
  msg += "   Por " + String(lastViaUdp ? "WiFi (UDP)" : "ESP-NOW") + ", ultimo mensaje hace " +
         String(ago) + " s\n";
  if (lastMinutePct >= 0) msg += "   Ultimo minuto: " + String(lastMinutePct, 0) + " % recibido\n";
  msg += "   Desde que arranco la sonda: " + String(pct(rxCount, lastSeq - firstSeq + 1), 0) + " % (" +
         String(rxCount) + " de " + String(lastSeq - firstSeq + 1) + ")\n";
  if (probeRssi != 0) {
    msg += "   WiFi de la sonda: " + String(probeRssi) + " dBm (" + rssiWord(probeRssi) + ")\n";
  }
  msg += "   Sonda encendida " + String(probeUptime / 60) + " min";
  if (!isnan(lastTemp)) msg += " | Agua abajo " + String(lastTemp, 1) + " C";
  if (!isnan(lastTemp2)) msg += ", medio " + String(lastTemp2, 1) + " C";
  // DS18B20 on the probe: how many it finds and what it last read
  msg += "\n   DS18B20: " + String(probeSensors) + " encontrada" + (probeSensors == 1 ? "" : "s");
  if (probeSensors == 0) {
    msg += " (revisa datos a G26, 3,3 V y GND)";
    if (probeLine & SONDA_LINE_CHECKED) {
      if (probeLine & SONDA_LINE_GROUND)
        msg += "\n   Linea de datos a masa: cable de datos y GND cruzados o en corto";
      else if (probeLine & SONDA_LINE_PULLUP)
        msg += "\n   Linea de datos: resistencia externa detectada (bien); revisa el 3,3 V";
      else
        msg += "\n   Linea de datos: SIN resistencia externa. Falta la de 4,7k entre datos (G26) y 3V3";
    }
  } else if (!isnan(probeRawTemp)) {
    float r = probeRawTemp;
    msg += ", ultima lectura " + String(r, 1) + " C";
    if (r <= -100) msg += " (no responde: falta la resistencia de 4,7k?)";
    else if (r == 85.0) msg += " (valor de arranque, aun sin medida)";
  }
  return msg;
}
