#include "BenyTask.h"
#include "config.h"
#include <Arduino.h>
#include <cstring> // For memset

WiFiUDP benyUdp;
BenyData benyData = {false, 0, 0, 0, 0, "DISCONNECTED", false};

unsigned long lastBenyPoll = 0;
const int benyPollInterval = 2000; // Poll every 2 seconds

// Helper to convert int to Hex String (padded)
String intToHex(int value, int digits) {
  char buf[20];
  memset(buf, 0, sizeof(buf));
  if (digits == 2)
    snprintf(buf, sizeof(buf), "%02x", value);
  else if (digits == 4)
    snprintf(buf, sizeof(buf), "%04x", value);
  else if (digits == 5)
    snprintf(buf, sizeof(buf), "%05x", value);
  else if (digits == 8)
    snprintf(buf, sizeof(buf), "%08x", value);
  else
    snprintf(buf, sizeof(buf), "%x", value);
  return String(buf);
}

// Calculate checksum: Sum of bytes % 256
// Msg is ASCII Hex string. We walk 2 chars at a time.
uint8_t calculateChecksum(String msg) {
  int sum = 0;
  for (int i = 0; i < msg.length(); i += 2) {
    String byteStr = msg.substring(i, i + 2);
    sum += (int)strtol(byteStr.c_str(), NULL, 16);
  }
  return sum % 256;
}

// Build Packet: Header + ID + Content + Checksum
// Based on python: 55aa (Header) ...
String buildPacket(String prefix, String pinHex, String suffix) {
  String packet = prefix + pinHex + suffix;
  uint8_t chk = calculateChecksum(packet);
  packet += intToHex(chk, 2);
  return packet;
}

void sendPacket(String packet) {
  if (PacketDebug)
    Serial.printf("Beny: Sending %s\n", packet.c_str());
  benyUdp.beginPacket(BENY_IP, BENY_PORT);
  benyUdp.print(packet); // Send as ASCII

  int result = benyUdp.endPacket();
  if (result == 0)
    Serial.println("Beny: ERROR sending packet (endPacket=0)");
}

void sendBroadcast(String packet) {
  if (PacketDebug)
    Serial.printf("Beny: Broadcast %s\n", packet.c_str());
  benyUdp.beginPacket("255.255.255.255", BENY_PORT);
  benyUdp.print(packet);
  benyUdp.endPacket();
}

void benyPollDevices() {
  String pinHex = intToHex(BENY_PIN, 5);
  int serialInt = atoi(BENY_SERIAL);
  String serialHex = intToHex(serialInt, 8);
  String packet = buildPacket("55aa03000f000", pinHex, "03" + serialHex);
  sendBroadcast(packet);
}

void setupBeny() {
  Serial.println("Beny: Initializing UDP...");
  Serial.printf("DEBUG: Configured PIN (Int): %d\n", BENY_PIN);
  Serial.println("DEBUG: Configured PIN (Hex): " + intToHex(BENY_PIN, 5));
  benyUdp.begin(BENY_PORT);

  // Send a poll immediately on startup
  delay(500);
  benyPollDevices();
}

void parseResponse(String response) {
  // Basic validation
  if (response.length() < 10)
    return;

  // Header 55aa (0-4)
  // unknown 1000 (4-8)
  // MsgType (8-10) - Fixed offset
  String msgTypeStr = response.substring(8, 10);
  int msgType = strtol(msgTypeStr.c_str(), NULL, 16);

  // Debug Message Type
  if (PacketDebug)
    Serial.printf("Beny: MsgType %d (0x%s)\n", msgType, msgTypeStr.c_str());

  if (msgType == 8) { // 0x08
    Serial.println("Beny: ACCESS DENIED! Check PIN.");
    benyData.status = "ACCESS DENIED";
    benyData.online = true;
  }

  // Python: SERVER_MESSAGE.SEND_VALUES_1P = 0x1E (30)
  if (msgType == 30) {
    // Adjusted Structure based on observation:
    // request_type: 10-12
    // skipped: 12-14
    // current: 14-16 (/10)
    // voltage: 18-20 (No Div, seems raw based on 0xED=237)
    // power: 20-24 (/10 assumed)
    // kwh: 24-30 (3 bytes? "000072" -> 114 -> 11.4kWh)
    // state: 30-32 ("01" -> UNPLUGGED, matches user reality)

    // User report: Power is 10x lower than expected.
    // If V=230 is correct, then I must be 10x lower.
    // Logic was: raw/10.0. If raw=20 (for 20A), result=2.0A. Power=460W
    // (0.46kW). Correct Power=4600W. Hypothesis: Current is sent as Integer
    // Amps (e.g. 20 for 20A), not decigrams.
    float amps =
        strtol(response.substring(14, 16).c_str(), NULL, 16); // Removed / 10.0
    float volts = strtol(response.substring(18, 20).c_str(), NULL, 16);

    // Debug Power Parsing
    String pwrHex = response.substring(20, 24);
    float rawWatts = strtol(pwrHex.c_str(), NULL, 16);

    // Fix: Calculate Power from V*I because raw power field is
    // unreliable/unknown scale
    float watts = volts * amps;

    // Always log RAW for now
    if (PacketDebug || true) {
      Serial.printf("Beny RAW: %s. PwrHex: %s -> %.0f. Calc W: %.1f\n",
                    response.c_str(), pwrHex.c_str(), rawWatts, watts);
    }

    // Try 3 bytes for kWh, see if it makes sense (Total Energy)
    // response.substring(24, 30) takes 6 chars -> 3 bytes
    float kwh = strtol(response.substring(24, 30).c_str(), NULL, 16) / 10.0;

    // State at 30-32
    int stateVal = strtol(response.substring(30, 32).c_str(), NULL, 16);

    benyData.current = amps;
    benyData.voltage = volts;
    benyData.power = watts;
    benyData.total_kwh = kwh;
    benyData.online = true;

    // States: 0=ABNORMAL, 1=UNPLUGGED, 2=STANDBY, 3=STARTING, 4=UNKNOWN,
    // 5=WAITING, 6=CHARGING
    switch (stateVal) {
    case 1:
      benyData.status = "UNPLUGGED";
      break;
    case 2:
      benyData.status = "STANDBY";
      break;
    case 3:
      benyData.status = "STARTING";
      break;
    case 5:
      benyData.status = "WAITING";
      break;
    case 6:
      benyData.status = "CHARGING";
      break;
    default:
      benyData.status = "ST:" + String(stateVal);
      break; // Fallback
    }

    Serial.printf("Beny: Data - %.1f W, %.1f V, %.1f A, %s, %.1f kWh\n", watts,
                  volts, amps, benyData.status.c_str(), kwh);
  }
}

void requestData() {
  String pinHex = intToHex(BENY_PIN, 5);
  String packet = buildPacket("55aa10000b000", pinHex, "70");
  sendPacket(packet);
}

void benyStartCharge() {
  String pinHex = intToHex(BENY_PIN, 5);
  String packet = buildPacket("55aa10000c000", pinHex, "0601");
  sendPacket(packet);
}

void benyStopCharge() {
  String pinHex = intToHex(BENY_PIN, 5);
  String packet = buildPacket("55aa10000c000", pinHex, "0600");
  sendPacket(packet);
}

void benySetCurrent(int amps) {
  // SET_VALUES (0x6d)
  // hex: 55aa10000d000[pin]6d00[max_amps][checksum]
  // max_amps: 2 hex chars (1 byte)
  if (amps < BENY_MIN_AMPS)
    amps = BENY_MIN_AMPS;
  if (amps > BENY_MAX_AMPS)
    amps = BENY_MAX_AMPS;

  String pinHex = intToHex(BENY_PIN, 5);
  String ampsHex = intToHex(amps, 2);

  // Prefix: 55aa10000d000
  // Suffix: 6d00 + ampsHex
  String packet = buildPacket("55aa10000d000", pinHex, "6d00" + ampsHex);
  sendPacket(packet);
}

void loopBeny() {
  // Handle Incoming
  int packetSize = benyUdp.parsePacket();
  if (packetSize) {
    String response = "";
    while (benyUdp.available()) {
      char c = (char)benyUdp.read();
      response += c;
    }
    if (PacketDebug)
      Serial.println("Beny: Rx " + response);
    parseResponse(response);
  }

  // Periodic Poll
  if (millis() - lastBenyPoll > benyPollInterval) {
    lastBenyPoll = millis();
    requestData();
    static int cnt = 0;
    if (cnt++ % 5 == 0)
      benyPollDevices();
  }

  // Debug Loop (Print every 10s to not spam too much)
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 10000) {
    lastDebug = millis();
    Serial.printf("DEBUG LOOP: PIN %d, Hex %s\n", BENY_PIN,
                  intToHex(BENY_PIN, 5).c_str());
  }
}

BenyData getBenyData() { return benyData; }
