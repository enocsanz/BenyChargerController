#ifndef TUYA_LOCAL_H
#define TUYA_LOCAL_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

class TuyaLocal {
public:
  TuyaLocal();
  void begin(const char *ip, const char *id, const char *uuid, const char *key,
             int version = 3);
  void handle(); // Call in loop

  void setSwitch(bool on);
  void update(); // Request status update

  // Getters
  bool getSwitchStatus();
  float getPower();
  float getVoltage();
  float getCurrent();
  bool isConnected();

private:
  const char *_ip_addr;
  const char *_dev_id;
  const char *_uuid; // Added UUID
  const char *_loc_key;
  int _version; // 3 for 3.3/3.4

  WiFiClient _client;
  unsigned long _last_poll;
  unsigned long _last_connect_attempt;
  bool _connected;

  // State
  bool _state;
  float _power;
  float _voltage;
  float _current;

  // Buffers
  uint8_t _rx_buf[1024];
  int _rx_len;

  // Helper functions
  void connect();
  void processIncoming();
  void parsePacket(uint8_t *data, int len);
  void parsePayload(uint8_t *payload, int len);

  void sendCommand(int cmd, const char *payload);
  void sendPacket(int cmd, uint8_t *payload, int len);

  // Encryption
  void encrypt_v33(uint8_t *output, const uint8_t *input, int len);
  void decrypt_v33(uint8_t *output, const uint8_t *input, int len);
};

#endif
