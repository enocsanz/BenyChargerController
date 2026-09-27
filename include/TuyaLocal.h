#ifndef TUYA_LOCAL_H
#define TUYA_LOCAL_H

#include <Arduino.h>
#include <WiFi.h>

// Minimal local client for Tuya protocols 3.4 and 3.5, with the session key
// negotiated on every connection. Only what a metering relay needs: read its
// DPs and set the switch. No cloud involved; the local key is obtained once
// with `python -m tinytuya wizard`.
//   3.4: 0x55AA frames, AES-128-ECB + HMAC-SHA256
//   3.5: 0x6699 frames, AES-128-GCM
class TuyaLocal {
public:
  // version: 34 or 35. name: only for the log.
  void begin(const char *name, const char *ip, const char *localKey, int version);
  void loop();             // Call often. Only (re)connecting blocks, <~7s.
  bool setSwitch(bool on); // false if there is no session
  bool connected() const { return _ready; }

  // Last values reported by the device (DP 1, 19, 20, 18)
  bool switchOn = false;
  float power = 0;              // W
  float voltage = 0;            // V
  float current = 0;            // A
  unsigned long lastUpdate = 0; // millis() of the last DP report, 0 = never

private:
  const char *_name = "Tuya";
  IPAddress _ip;
  int _version = 35;
  uint8_t _localKey[16];
  uint8_t _sessionKey[16];
  const uint8_t *_key = _localKey; // key for the frames in flight
  uint8_t _localNonce[16];

  WiFiClient _client;
  bool _ready = false;
  uint32_t _seq = 0;
  unsigned long _lastAttempt = 0;
  bool _attempted = false;
  unsigned long _lastPoll = 0;
  unsigned long _lastRx = 0;

  uint8_t _rx[1024];
  size_t _rxLen = 0;
  uint8_t _pt[1024];

  bool connect();
  void close(const char *why);
  bool send(uint32_t cmd, const uint8_t *payload, size_t len);
  bool send34(uint32_t cmd, const uint8_t *payload, size_t len);
  bool send35(uint32_t cmd, const uint8_t *payload, size_t len);
  void pump();
  bool readFrame(uint32_t &cmd, size_t &ptLen);
  int readFrame34(uint32_t &cmd, size_t &ptLen);
  int readFrame35(uint32_t &cmd, size_t &ptLen);
  void handlePayload(const uint8_t *pt, size_t len);
};

#endif
