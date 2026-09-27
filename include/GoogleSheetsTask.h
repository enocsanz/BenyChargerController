#ifndef GOOGLE_SHEETS_TASK_H
#define GOOGLE_SHEETS_TASK_H

#include <Arduino.h>

void setupGoogleSheets();
void loopGoogleSheets();

// Diagnostic log: one sample per minute ("Muestras") plus events ("Eventos"),
// sent in batches. Safe to call from anywhere: it only queues the text.
void logEvent(const char *type, const String &detail);
void logEventf(const char *type, const char *fmt, ...);
void setDiagEnabled(bool on);
String diagStatusText(); // For Telegram

extern bool diag_enabled;

#endif
