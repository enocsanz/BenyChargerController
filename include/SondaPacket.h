#ifndef SONDA_PACKET_H
#define SONDA_PACKET_H

#include <stdint.h>

// ESP-NOW message from the probe next to the water heater (M5StickC Plus) to
// the main controller (StampS3). Shared by both firmwares: the probe project
// in sonda/ includes this same file.
#define SONDA_MAGIC 0x534F4E44 // "SOND"

struct __attribute__((packed)) SondaPacket {
  uint32_t magic;  // SONDA_MAGIC
  uint32_t seq;    // +1 per message; restarts at 1 when the probe reboots
  uint32_t uptime; // probe uptime (s)
  float temp;      // water temperature (C); NAN while there is no sensor
};

#endif
