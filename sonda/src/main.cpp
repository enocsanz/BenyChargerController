// Probe next to the water heater (M5StickC Plus). For now a link test: sends
// one numbered message per second to the main controller over ESP-NOW and
// shows on screen how many it confirmed, to find a spot where the link holds.
// Later it will also carry the DS18B20 water temperature.

#include "SondaPacket.h"
#include <M5StickCPlus.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// Main controller's WiFi MAC (StampS3). /espnow on Telegram shows it.
static uint8_t MAIN_MAC[6] = {0x48, 0x27, 0xE2, 0xE3, 0x1A, 0x3C};

// ESP-NOW has to use the router's channel, which the controller is on. The
// probe does not join the WiFi: it tries channels until the controller
// confirms, and searches again if it stops confirming (the router may move).
static const int RESCAN_AFTER = 10; // consecutive unconfirmed messages

static int channel = 0;            // 0 = searching
static volatile int sendResult = -1; // -1 pending, 0 fail, 1 confirmed
static uint32_t seq = 0, sent = 0, acked = 0, failsInRow = 0;
static uint32_t minSent = 0, minAcked = 0;
static float lastMinutePct = -1;

static void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  sendResult = (status == ESP_NOW_SEND_SUCCESS) ? 1 : 0;
}

// Sends one message and waits for the MAC-layer confirmation
static bool sendOne() {
  SondaPacket p;
  p.magic = SONDA_MAGIC;
  p.seq = ++seq;
  p.uptime = millis() / 1000;
  p.temp = NAN; // no sensor yet
  sendResult = -1;
  if (esp_now_send(MAIN_MAC, (uint8_t *)&p, sizeof(p)) != ESP_OK) return false;
  unsigned long t0 = millis();
  while (sendResult < 0 && millis() - t0 < 100) delay(1);
  return sendResult == 1;
}

static void setChannel(int ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

// Tries every channel; stays on the first one the controller confirms
static void searchChannel() {
  for (int ch = 1; ch <= 13; ch++) {
    setChannel(ch);
    for (int i = 0; i < 3; i++) {
      if (sendOne()) {
        channel = ch;
        failsInRow = 0;
        Serial.printf("Canal %d: el controlador responde\n", ch);
        return;
      }
    }
  }
  channel = 0;
}

static void draw() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.println("Sonda termo");
  if (channel == 0) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.println("BUSCANDO...");
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.printf("Enviados %u\n", sent);
    return;
  }
  M5.Lcd.printf("Canal %d\n", channel);
  if (lastMinutePct >= 0) {
    uint16_t c = lastMinutePct >= 80 ? GREEN : lastMinutePct >= 50 ? YELLOW : RED;
    M5.Lcd.setTextColor(c, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.printf("%3.0f%%\n", lastMinutePct);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE, BLACK);
  } else {
    M5.Lcd.println("midiendo...");
  }
  M5.Lcd.printf("Total %.0f%%\n", sent ? 100.0 * acked / sent : 0);
  M5.Lcd.printf("%u/%u\n", acked, sent);
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(9);
  Serial.begin(115200);

  // The probe must stay on the channel it set. This board kept the WiFi
  // credentials of an old firmware, and the auto-connect kept scanning, i.e.
  // hopping channels: only 25 % of the messages got through, side by side.
  // Erase them and never connect.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, true); // true: erase the stored credentials
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (esp_now_init() != ESP_OK) {
    M5.Lcd.println("ESP-NOW FALLO");
    while (true) delay(1000);
  }
  esp_now_register_send_cb(onSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAIN_MAC, 6);
  peer.channel = 0; // the current one, set by setChannel()
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  draw();
  searchChannel();
}

void loop() {
  static unsigned long lastSend = 0, lastMinute = 0, lastDraw = 0;

  if (channel == 0) {
    searchChannel();
    draw();
    delay(2000);
    return;
  }

  if (millis() - lastSend >= 1000) {
    lastSend = millis();
    bool ok = sendOne();
    sent++;
    if (ok) {
      acked++;
      failsInRow = 0;
    } else if (++failsInRow >= RESCAN_AFTER) {
      Serial.println("Sin confirmacion: buscando canal otra vez");
      channel = 0;
    }
  }

  if (millis() - lastMinute >= 60000) {
    lastMinute = millis();
    uint32_t s = sent - minSent;
    lastMinutePct = s ? 100.0 * (acked - minAcked) / s : -1;
    minSent = sent;
    minAcked = acked;
    Serial.printf("Ultimo minuto: %.0f%% | total %u/%u\n", lastMinutePct, acked, sent);
  }

  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    draw();
  }
  delay(5);
}
