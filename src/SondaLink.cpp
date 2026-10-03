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

String sondaLinkText() {
  if (!started) return "📡 Sonda: enlace no iniciado";
  String msg = "📡 Sonda del termo\n";
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
    const char *q = probeRssi >= -67 ? "buena" : probeRssi >= -75 ? "aceptable"
                    : probeRssi >= -80 ? "justa" : "mala";
    msg += "   WiFi de la sonda: " + String(probeRssi) + " dBm (" + q + ")\n";
  }
  msg += "   Sonda encendida " + String(probeUptime / 60) + " min";
  if (!isnan(lastTemp)) msg += " | Agua " + String(lastTemp, 1) + " C";
  return msg;
}
