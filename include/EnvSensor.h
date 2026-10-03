#ifndef ENV_SENSOR_H
#define ENV_SENSOR_H

#include <Arduino.h>

// M5Stack ENV III unit (SHT30 temperature/humidity at 0x44, QMP6988 pressure
// at 0x70) on the StampS3. The StampS3 has no Grove socket, so the unit is
// wired to two free pins: the first boot tries the free pin pairs until the
// SHT30 answers, and keeps the pair it found.
void setupEnvSensor(); // after WiFi (the result is reported on Telegram)
void loopEnvSensor();  // reads every 30 s
bool envSensorOk();
float envTemp();     // C, NAN without a reading
float envHumidity(); // %, NAN without a reading
String envText();    // one line for /status
String envI2cDiag(); // /i2c: line levels, devices on the bus, last command

#endif
