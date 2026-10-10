#ifndef SONDA_PACKET_H
#define SONDA_PACKET_H

#include <stdint.h>

// Message from the probe next to the water heater (M5StickC Plus) to the main
// controller (StampS3), over the home WiFi (UDP) or ESP-NOW. Shared by both firmwares: the probe project
// in sonda/ includes this same file.
#define SONDA_MAGIC 0x534F4E44 // "SOND"
#define SONDA_LINE_PULLUP 0x01  // data line high against the internal pull-down: external pull-up present
#define SONDA_LINE_GROUND 0x02  // data line low even with the internal pull-up: shorted / miswired
#define SONDA_LINE_CHECKED 0x80 // the check was done
#define SONDA_UDP_PORT 4210    // over the home WiFi; the controller echoes each message

struct __attribute__((packed)) SondaPacket {
  uint32_t magic;  // SONDA_MAGIC
  uint32_t seq;    // +1 per message; restarts at 1 when the probe reboots
  uint32_t uptime; // probe uptime (s)
  float temp;      // water at the bottom: thermostat's well (C); NAN without a reading
  int8_t rssi;     // probe's WiFi signal (dBm); 0 over ESP-NOW
  char fw[21];     // probe firmware build, __DATE__ " " __TIME__
  uint8_t sensors; // DS18B20 found on the bus
  float rawTemp;   // last reading as read, valid or not (-127: no answer)
  uint8_t line;    // data line check, see SONDA_LINE_*
  float temp2;     // water at mid height: old heating coil (C); NAN without a reading
};

#endif
