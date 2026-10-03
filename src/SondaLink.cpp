#include "SondaLink.h"
#include "GoogleSheetsTask.h"
#include "SondaPacket.h"
#include <WiFi.h>
#include <esp_now.h>

// The probe sends one message per second, numbered. Delivery is measured from
// the sequence numbers: received / (last seq - first seq + 1). The receive
// callback runs in the WiFi task, so it only updates these counters.
static volatile uint32_t rxCount = 0;  // messages received since the probe's boot
static volatile uint32_t firstSeq = 0; // first seq seen since the probe's boot
static volatile uint32_t lastSeq = 0;
static volatile unsigned long lastRxAt = 0;
static volatile float lastTemp = NAN;
static volatile uint32_t probeUptime = 0;
static bool started = false;

// Last-minute and last-hour snapshots
static uint32_t minRx = 0, minSeq = 0, hourRx = 0, hourSeq = 0;
static float lastMinutePct = -1;

static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != sizeof(SondaPacket)) return;
  SondaPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != SONDA_MAGIC) return;
  if (p.seq < lastSeq || rxCount == 0) { // probe rebooted: start counting again
    rxCount = 0;
    firstSeq = p.seq;
    minRx = hourRx = 0;
    minSeq = hourSeq = p.seq - 1;
  }
  rxCount = rxCount + 1;
  lastSeq = p.seq;
  lastRxAt = millis();
  lastTemp = p.temp;
  probeUptime = p.uptime;
}

void setupSondaLink() {
  if (started) return;
  if (esp_now_init() != ESP_OK) {
    logEvent("SONDA", "ESP-NOW no se pudo iniciar");
    return;
  }
  esp_now_register_recv_cb(onRecv);
  started = true;
  Serial.printf("ESP-NOW: escuchando en canal %d, MAC %s\n", WiFi.channel(),
                WiFi.macAddress().c_str());
}

static float pct(uint32_t rx, uint32_t seqSpan) {
  return seqSpan ? 100.0 * rx / seqSpan : -1;
}

void loopSondaLink() {
  if (!started) return;
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
      logEventf("SONDA", "Ultima hora: %.0f %% recibido (%u de %u)", pct(rx - hourRx, seq - hourSeq),
                rx - hourRx, seq - hourSeq);
    } else {
      logEvent("SONDA", "Ultima hora: ningun mensaje de la sonda");
    }
    hourRx = rx;
    hourSeq = seq;
  }
}

String sondaLinkText() {
  if (!started) return "📡 Sonda (ESP-NOW): no iniciado";
  String msg = "📡 Sonda (ESP-NOW), canal " + String(WiFi.channel()) + "\n";
  if (rxCount == 0) return msg + "   Ningun mensaje recibido todavia\n   MAC de este controlador: " +
                           WiFi.macAddress();
  unsigned long ago = (millis() - lastRxAt) / 1000;
  msg += "   Ultimo mensaje hace " + String(ago) + " s\n";
  if (lastMinutePct >= 0) msg += "   Ultimo minuto: " + String(lastMinutePct, 0) + " % recibido\n";
  msg += "   Desde que arranco la sonda: " + String(pct(rxCount, lastSeq - firstSeq + 1), 0) + " % (" +
         String(rxCount) + " de " + String(lastSeq - firstSeq + 1) + ")\n";
  msg += "   Sonda encendida " + String(probeUptime / 60) + " min";
  if (!isnan(lastTemp)) msg += " | Agua " + String(lastTemp, 1) + " C";
  return msg;
}
