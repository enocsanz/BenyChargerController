#include "TuyaLocal.h"
#include <ArduinoJson.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

// Frame layout (checked against the Tongou relay, see README):
//   00 00 66 99 | 00 00 | seq(4) | cmd(4) | len(4) | iv(12) | data | tag(16) | 00 00 99 66
// len counts iv + data + tag; the GCM additional data is the 14 bytes between
// the prefix and the iv. Everything the device sends starts with a 4-byte
// return code, and status pushes add a "3.5" + 12 bytes header before the JSON.

static const uint16_t TUYA_PORT = 6668;
static const uint32_t CMD_SESS_START = 3;
static const uint32_t CMD_SESS_RESP = 4;
static const uint32_t CMD_SESS_FINISH = 5;
static const uint32_t CMD_CONTROL = 13;
static const uint32_t CMD_QUERY = 16;

static const unsigned long POLL_INTERVAL = 10000;  // DP query
static const unsigned long RX_TIMEOUT = 30000;     // silence -> reconnect
static const unsigned long RETRY_INTERVAL = 30000; // between connect attempts
static const int32_t CONNECT_TIMEOUT = 3000;       // ms
static const unsigned long STEP_TIMEOUT = 2000;    // per negotiation step

static inline uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void putBe32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static bool gcmEncrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *aad, size_t aadLen,
                       const uint8_t *in, size_t len, uint8_t *out, uint8_t *tag) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int r = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (r == 0)
    r = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len, iv, 12, aad, aadLen, in, out,
                                  16, tag);
  mbedtls_gcm_free(&ctx);
  return r == 0;
}

static bool gcmDecrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *aad, size_t aadLen,
                       const uint8_t *in, size_t len, const uint8_t *tag, uint8_t *out) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int r = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (r == 0)
    r = mbedtls_gcm_auth_decrypt(&ctx, len, iv, 12, aad, aadLen, tag, 16, in, out);
  mbedtls_gcm_free(&ctx);
  return r == 0;
}

static void hmacSha256(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t *out) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 16, msg, len, out);
}

void TuyaLocal::begin(const char *ip, const char *localKey) {
  _ip.fromString(ip);
  memcpy(_localKey, localKey, 16);
}

bool TuyaLocal::send(uint32_t cmd, const uint8_t *payload, size_t len) {
  uint8_t frame[18 + 12 + 256 + 16 + 4];
  if (len > 256) return false;

  uint8_t *hdr = frame;
  putBe32(hdr, 0x00006699);
  hdr[4] = hdr[5] = 0;
  putBe32(hdr + 6, ++_seq);
  putBe32(hdr + 10, cmd);
  putBe32(hdr + 14, 12 + len + 16);

  uint8_t *iv = frame + 18;
  esp_fill_random(iv, 12);
  uint8_t *data = iv + 12;
  if (!gcmEncrypt(_key, iv, hdr + 4, 14, payload, len, data, data + len)) return false;
  putBe32(data + len + 16, 0x00009966);

  size_t total = 18 + 12 + len + 16 + 4;
  return _client.write(frame, total) == total;
}

void TuyaLocal::pump() {
  int avail = _client.available();
  while (avail > 0 && _rxLen < sizeof(_rx)) {
    int n = _client.read(_rx + _rxLen, min((size_t)avail, sizeof(_rx) - _rxLen));
    if (n <= 0) break;
    _rxLen += n;
    avail = _client.available();
  }
}

// Extracts one complete frame from the rx buffer, decrypted into _pt.
bool TuyaLocal::readFrame(uint32_t &cmd, size_t &ptLen) {
  while (true) {
    if (_rxLen < 18) return false;
    if (be32(_rx) != 0x00006699) {
      // Out of sync: drop a byte and look for the next prefix
      memmove(_rx, _rx + 1, --_rxLen);
      continue;
    }
    uint32_t len = be32(_rx + 14);
    if (len < 28 || 18 + len + 4 > sizeof(_rx)) {
      _rxLen = 0; // Garbage or oversized, start over
      return false;
    }
    size_t total = 18 + len + 4;
    if (_rxLen < total) return false;

    cmd = be32(_rx + 10);
    ptLen = len - 28;
    bool ok = gcmDecrypt(_key, _rx + 18, _rx + 4, 14, _rx + 30, ptLen, _rx + 30 + ptLen, _pt);
    memmove(_rx, _rx + total, _rxLen - total);
    _rxLen -= total;
    if (ok) {
      _lastRx = millis();
      return true;
    }
    Serial.printf("Tuya: Trama cmd %u no descifra, descartada\n", cmd);
  }
}

bool TuyaLocal::connect() {
  Serial.printf("Tuya: Conectando a %s...\n", _ip.toString().c_str());
  if (!_client.connect(_ip, TUYA_PORT, CONNECT_TIMEOUT)) {
    Serial.println("Tuya: Sin respuesta del rele");
    return false;
  }
  _client.setNoDelay(true);
  _rxLen = 0;
  _key = _localKey;

  // 1. Our nonce, encrypted with the local key
  esp_fill_random(_localNonce, 16);
  if (!send(CMD_SESS_START, _localNonce, 16)) {
    close("fallo al enviar");
    return false;
  }

  // 2. Device nonce + HMAC of ours (proves both sides share the local key)
  unsigned long start = millis();
  uint32_t cmd;
  size_t ptLen;
  bool got = false;
  while (millis() - start < STEP_TIMEOUT) {
    pump();
    if (readFrame(cmd, ptLen) && cmd == CMD_SESS_RESP) {
      got = true;
      break;
    }
    delay(10);
  }
  if (!got || ptLen < 52) {
    close("sin respuesta a la negociacion (clave local incorrecta?)");
    return false;
  }
  uint8_t remoteNonce[16];
  memcpy(remoteNonce, _pt + 4, 16); // skip the return code
  uint8_t check[32];
  hmacSha256(_localKey, _localNonce, 16, check);
  if (memcmp(check, _pt + 20, 32) != 0) {
    close("HMAC incorrecto (clave local incorrecta)");
    return false;
  }

  // 3. HMAC of the device nonce, then derive the session key:
  //    GCM(localKey, iv = localNonce[0..11], localNonce XOR remoteNonce)
  uint8_t mac[32];
  hmacSha256(_localKey, remoteNonce, 16, mac);
  if (!send(CMD_SESS_FINISH, mac, 32)) {
    close("fallo al enviar");
    return false;
  }
  uint8_t mix[16], tag[16];
  for (int i = 0; i < 16; i++) mix[i] = _localNonce[i] ^ remoteNonce[i];
  gcmEncrypt(_localKey, _localNonce, nullptr, 0, mix, 16, _sessionKey, tag);
  _key = _sessionKey;

  _ready = true;
  _lastRx = millis();
  _lastPoll = 0; // query right away
  Serial.println("Tuya: Sesion establecida");
  return true;
}

void TuyaLocal::close(const char *why) {
  Serial.printf("Tuya: Desconectado (%s)\n", why);
  _client.stop();
  _ready = false;
  _rxLen = 0;
}

void TuyaLocal::handlePayload(const uint8_t *pt, size_t len) {
  // Acks carry only the return code; data frames have JSON after the headers
  const uint8_t *json = (const uint8_t *)memchr(pt, '{', len);
  if (!json) return;

  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, (const char *)json, len - (json - pt))) return;
  JsonObject dps = doc["dps"]; // DP query reply
  if (dps.isNull()) dps = doc["data"]["dps"]; // status push
  if (dps.isNull()) return;

  bool wasOn = switchOn;
  if (dps.containsKey("1")) switchOn = dps["1"];
  if (dps.containsKey("19")) power = dps["19"].as<float>() / 10.0;
  if (dps.containsKey("20")) voltage = dps["20"].as<float>() / 10.0;
  if (dps.containsKey("18")) current = dps["18"].as<float>() / 1000.0;
  if (lastUpdate == 0 || switchOn != wasOn) {
    Serial.printf("Tuya: Rele %s, %.0f W, %.1f V\n", switchOn ? "ON" : "OFF", power, voltage);
  }
  lastUpdate = millis();
}

void TuyaLocal::loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (_ready) close("sin WiFi");
    return;
  }

  if (!_ready) {
    if (!_attempted || millis() - _lastAttempt > RETRY_INTERVAL) {
      _attempted = true;
      _lastAttempt = millis();
      connect();
    }
    return;
  }

  if (!_client.connected()) {
    close("conexion cerrada por el rele");
    return;
  }

  pump();
  uint32_t cmd;
  size_t ptLen;
  while (readFrame(cmd, ptLen)) {
    handlePayload(_pt, ptLen);
  }

  if (millis() - _lastRx > RX_TIMEOUT) {
    close("sin respuesta");
    return;
  }

  if (_lastPoll == 0 || millis() - _lastPoll > POLL_INTERVAL) {
    _lastPoll = millis();
    send(CMD_QUERY, (const uint8_t *)"{}", 2);
  }
}

bool TuyaLocal::setSwitch(bool on) {
  if (!_ready) return false;
  // CONTROL_NEW payload: "3.5" + 12 zero bytes, then the JSON
  uint8_t payload[160] = {'3', '.', '5'};
  int n = snprintf((char *)payload + 15, sizeof(payload) - 15,
                   "{\"protocol\":5,\"t\":%lu,\"data\":{\"dps\":{\"1\":%s}}}",
                   (unsigned long)time(nullptr), on ? "true" : "false");
  return send(CMD_CONTROL, payload, 15 + n);
}
