#include "TelegramTask.h"
#include "BenyTask.h"
#include "GoogleSheetsTask.h"
#include "PiscinaTask.h"
#include "TermoTask.h"

// #include "WeatherTask.h" REMOVED

#include "config.h"
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>

// Externs from main.cpp
extern float getCurrentPrice();
extern int32_t current_grid_power;
extern int32_t current_pv_power;
extern int charging_mode;       // 0=Solar, 1=Balanceo
extern int max_grid_power;      // Added for dynamic limit
extern int target_amps;         // Current DLB setpoint (A)
extern void saveMode(int mode); // Added for persistence
extern void saveMaxGridPower(int watts);

WiFiClientSecure clientTCP;
UniversalTelegramBot bot(BOT_TOKEN, clientTCP);

unsigned long lastTelegramTime = 0;
const int telegramInterval = 2000;

// Outgoing notification queue.
// sendTelegramNotification() is called from the main loop and from setup(),
// where a blocking HTTPS request would stall the loop for seconds and risk a
// WDT reset. So callers only leave the text here; loopTelegram() does the
// actual send, in the task that already owns the network client.
const int NOTIFY_QUEUE_SIZE = 4;
String notifyQueue[NOTIFY_QUEUE_SIZE];
int notifyCount = 0;

void setupTelegram() {
  clientTCP.setInsecure();
  clientTCP.setHandshakeTimeout(20000);
}

String last_chat_id = ""; // Store for proactive notifications

// WiFi signal with a plain-words rating, to place the controller well
static String wifiText() {
  long rssi = WiFi.RSSI();
  const char *q = rssi >= -67 ? "buena" : rssi >= -75 ? "aceptable" : rssi >= -80 ? "justa" : "mala";
  return String(rssi) + " dBm (" + q + ")";
}

// One line about the controller itself: WiFi, uptime, free memory
static String systemText() {
  unsigned long min = millis() / 60000;
  String up = min >= 1440 ? String(min / 1440) + " d " + String((min / 60) % 24) + " h"
                          : String(min / 60) + " h " + String(min % 60) + " min";
  return "📶 WiFi " + wifiText() + " | Encendido " + up + " | Memoria libre " +
         String(ESP.getFreeHeap() / 1024) + " KB";
}

void handleNewMessages(int numNewMessages) {
  Serial.println("Telegram: Handling messages...");

  for (int i = 0; i < numNewMessages; i++) {
    // Save Chat ID for notifications
    String chat_id = String(bot.messages[i].chat_id);
    last_chat_id = chat_id;

    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;
    String textLower = text;
    textLower.toLowerCase();
    logEvent("TELEGRAM", text);

    if (textLower == "/start" || textLower == "/help") {
      String msg = "Control Beny V2\n\n";
      msg += "📊 /status - Ver potencias, modo y amperaje actual\n\n";
      msg += "MODOS DE CARGA:\n";
      msg += "🔋 SOLAR: /solar - Carga con excedentes, ajustando el amperaje para no importar de red.\n";
      msg += "⚖️ BALANCEO: /balanceo - Carga dinamica aprovechando hasta el limite de red.\n\n";
      msg += "En ambos modos el amperaje se ajusta entre " + String(BENY_MIN_AMPS) +
             "A y " + String(BENY_MAX_AMPS) + "A. El minimo de " + String(BENY_MIN_AMPS) +
             "A es el suelo del cargador: si no hay sol, ese consumo se toma de la red.\n\n";
      msg += "AJUSTES: \n";
      msg += "Limite Red: /set_limit W (Objetivo del modo Balanceo)\n";
      msg += "Precio: /set_price E/kWh (Solo informativo, colorea la pantalla)\n\n";
      msg += "TERMO (ACS):\n";
      msg += "/termo - Estado del termo\n";
      msg += "/termo_auto - Corta por precio y por sobrecarga\n";
      msg += "/termo_on - Ignora el precio (sigue cortando por sobrecarga)\n";
      msg += "/termo_off - Apagado\n";
      msg += "/set_termo_precio E/kWh - Precio a partir del cual se corta\n\n";
      msg += "DEPURADORA (PISCINA):\n";
      msg += "/piscina - Estado y horas de hoy\n";
      msg += "/piscina_auto - Con sol, con maximo y minimo diarios\n";
      msg += "/piscina_on - Encendida\n";
      msg += "/piscina_off - Apagada\n";
      msg += "/set_piscina_horas MAX MIN - Horas de este mes (ej: /set_piscina_horas 6 3)\n\n";
      msg += "DIAGNOSTICO:\n";
      msg += "/diag - Estado del registro en Google Sheets\n";
      msg += "/wifi - Intensidad de la senal WiFi del controlador\n";
      msg += "/diag_on /diag_off - Activa o para el registro por minuto\n";
      bot.sendMessage(chat_id, msg, "");
    } else if (textLower == "/status") {
      String msg = "📊 ESTADO DEL SISTEMA \n\n";

      msg += "💰 Precio Luz (PVPC): " + String(getCurrentPrice(), 3) + " E/kWh\n\n";

      msg += "🏠 Red (Grid): " + String(current_grid_power > 0 ? "+" : "") + String((float)current_grid_power, 0) +
             " W / " + String(max_grid_power) + " W\n";
      msg += "   (+ Importando / - Exportando)\n\n";
      msg += "☀️ Solar: " + String(current_pv_power) + " W\n\n";

      BenyData bd = getBenyData();
      msg += "🔌 Cargador Beny: " + String(bd.power, 1) + " W\n";
      msg += "   STATUS: " + bd.status + "\n";
      msg += "   AMPERAJE: " + String(target_amps) + "A objetivo / " +
             String(bd.current, 0) + "A real\n\n";

      msg += "🚀 MODO ACTIVO: ";
      if (charging_mode == 0) {
        msg += "SOLAR\n";
        msg += "   Solo Excedentes (min " + String(BENY_MIN_AMPS) + "A)";
      } else if (charging_mode == 1) {
        msg += "BALANCEO\n";
        msg += "   Carga Dinamica (Max Red " + String(max_grid_power) + "W)";
      }
      msg += "\n";

      if (target_amps <= BENY_MIN_AMPS && (bd.status == "CHARGING" || bd.status == "STARTING")) {
        msg += "⚠️ Cargando al minimo (" + String(BENY_MIN_AMPS) +
               "A): el DLB no puede bajar mas.\n";
      }
      msg += "\n" + termoStatusText() + "\n";
      msg += "\n" + piscinaStatusText() + "\n";
      msg += "\n" + systemText() + "\n";

      bot.sendMessage(chat_id, msg, "");

    } else if (textLower.startsWith("/set_price ")) {
      String val_str = text.substring(11);
      float val = val_str.toFloat();
      if (val > 0) {
        max_price_threshold = val;
        // is_planned_for_tomorrow = false; REMOVED
        manual_logic_trigger = true; // Run logic now

        bot.sendMessage(chat_id,
                        "Umbral de precio actualizado a " +
                            String(max_price_threshold) +
                            " E/kWh. Recalculando...",
                        "");
      } else {
        bot.sendMessage(chat_id, "Valor invalido (usa . para decimales)", "");
      }
    } else if (textLower.startsWith("/set_limit ")) {
      String val_str = text.substring(11); // Length of "/set_limit "
      int val = val_str.toInt();
      if (val >= 1000 && val <= 10000) {
        max_grid_power = val;
        saveMaxGridPower(val);
        manual_logic_trigger = true; // Run logic now

        bot.sendMessage(chat_id, "Limite de Red actualizado a " + String(val) + " W.", "");
      } else {
        bot.sendMessage(chat_id, "Valor invalido (Min 1000, Max 10000)", "");
      }
    } else if (textLower.startsWith("/set_pausa ") ||
               textLower.startsWith("/set_reinicio ") ||
               textLower.startsWith("/set_margen ")) {
      bot.sendMessage(chat_id,
                      "❌ La pausa automatica ha sido eliminada: el cargador esta "
                      "en 'Plug and Charge' e ignoraba la orden de STOP, "
                      "rearrancando a plena potencia. Ahora el consumo se regula "
                      "solo con el amperaje (" + String(BENY_MIN_AMPS) + "-" +
                      String(BENY_MAX_AMPS) + "A) via /set_limit.", "");
    } else if (textLower == "/turbo") {
      bot.sendMessage(chat_id, "❌ El modo Turbo ha sido eliminado.", "");
    } else if (textLower == "/solar") {
      charging_mode = 0;
      saveMode(charging_mode);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Modo: SOLAR (Solo Excedentes).", "");
    } else if (textLower == "/balanceo") {
      charging_mode = 1;
      saveMode(charging_mode);
      manual_logic_trigger = true;
      bot.sendMessage(
          chat_id, "Modo: BALANCEO (Lim Red " + String(max_grid_power) + "W).", "");
    } else if (textLower == "/termo") {
      bot.sendMessage(chat_id, termoStatusText(), "");
    } else if (textLower == "/termo_auto" || textLower == "/termo auto") {
      setTermoMode(TERMO_AUTO);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Termo: AUTO (se corta con precio > " +
                                   String(termo_max_price, 3) + " E/kWh o sobrecarga).", "");
    } else if (textLower == "/termo_on" || textLower == "/termo on") {
      setTermoMode(TERMO_ON);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Termo: ON (ignora el precio; se sigue cortando por sobrecarga). "
                               "Vuelve con /termo_auto.", "");
    } else if (textLower == "/termo_off" || textLower == "/termo off") {
      setTermoMode(TERMO_OFF);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Termo: OFF. Vuelve con /termo_auto.", "");
    } else if (textLower.startsWith("/set_termo_precio ")) {
      float val = text.substring(18).toFloat(); // Length of "/set_termo_precio "
      if (val > 0 && val < 1) {
        setTermoMaxPrice(val);
        manual_logic_trigger = true;
        bot.sendMessage(chat_id, "Termo: se corta con precio > " + String(val, 3) + " E/kWh.", "");
      } else {
        bot.sendMessage(chat_id, "Valor invalido (E/kWh, entre 0 y 1, usa . para decimales)", "");
      }
    } else if (textLower == "/piscina") {
      bot.sendMessage(chat_id, piscinaStatusText(), "");
    } else if (textLower == "/piscina_auto" || textLower == "/piscina auto") {
      setPiscinaMode(PISCINA_AUTO);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Depuradora: AUTO (con sol, maximo y minimo diarios).", "");
    } else if (textLower == "/piscina_on" || textLower == "/piscina on") {
      setPiscinaMode(PISCINA_ON);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Depuradora: ON. Vuelve con /piscina_auto.", "");
    } else if (textLower == "/piscina_off" || textLower == "/piscina off") {
      setPiscinaMode(PISCINA_OFF);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Depuradora: OFF. Vuelve con /piscina_auto.", "");
    } else if (textLower.startsWith("/set_piscina_horas ")) {
      String args = text.substring(19); // Length of "/set_piscina_horas "
      args.trim();
      int sp = args.indexOf(' ');
      float maxH = args.substring(0, sp < 0 ? args.length() : sp).toFloat();
      float minH = sp < 0 ? -1 : args.substring(sp + 1).toFloat();
      if (sp > 0 && setPiscinaHours(maxH, minH)) {
        bot.sendMessage(chat_id, "Depuradora: este mes maximo " + String(maxH, 1) +
                                     " h y minimo " + String(minH, 1) + " h al dia.", "");
      } else {
        bot.sendMessage(chat_id, "Uso: /set_piscina_horas MAX MIN (horas, MIN <= MAX <= 24). "
                                 "Ej: /set_piscina_horas 6 3", "");
      }
    } else if (textLower == "/wifi") {
      bot.sendMessage(chat_id, "📶 WiFi: " + wifiText() + "\n   Red " + WiFi.SSID() + ", canal " +
                                   String(WiFi.channel()) + ", IP " + WiFi.localIP().toString() +
                                   "\n   Buena > -67, aceptable > -75, justa > -80 dBm", "");
    } else if (textLower == "/diag") {
      bot.sendMessage(chat_id, diagStatusText(), "");
    } else if (textLower == "/diag_on" || textLower == "/diag on") {
      setDiagEnabled(true);
      bot.sendMessage(chat_id, "Diagnostico ACTIVO: una muestra por minuto y eventos a Google "
                               "Sheets, enviados cada 5 min.", "");
    } else if (textLower == "/diag_off" || textLower == "/diag off") {
      setDiagEnabled(false);
      bot.sendMessage(chat_id, "Diagnostico desactivado. El registro horario sigue.", "");
    } else if (textLower == "/off" || textLower == "/stop") {
      bot.sendMessage(chat_id, "❌ Comando /off desactivado (Modo 'Plug and Charge' activo en el cargador).", "");
    }
  }
}

// Queues a notification. Safe to call from anywhere: it never touches the
// network, so it cannot block the main loop or trip the watchdog.
void sendTelegramNotification(String msg) {
  if (notifyCount >= NOTIFY_QUEUE_SIZE) {
    Serial.println("Tele: Notify queue full, dropping: " + msg);
    return;
  }
  notifyQueue[notifyCount++] = msg;
}

// Delivers one queued notification per call, so a burst never turns into a
// long chain of blocking HTTPS requests inside a single loop iteration.
void flushNotifications() {
  if (notifyCount == 0) return;

  String msg = notifyQueue[0];
  for (int i = 1; i < notifyCount; i++) {
    notifyQueue[i - 1] = notifyQueue[i];
  }
  notifyQueue[--notifyCount] = "";

  String target = (last_chat_id != "") ? last_chat_id : String(CHAT_ID);
  if (target == "") {
    Serial.println("Tele: No ID to notify.");
    return;
  }
  bot.sendMessage(target, msg, "");
}

void loopTelegram() {
  static unsigned long lastHeapLog = 0;
  if (millis() - lastHeapLog > 10000) {
    lastHeapLog = millis();
    Serial.printf("Telegram: Polling... (Heap: %d)\n", ESP.getFreeHeap());
  }

  if (millis() - lastTelegramTime > telegramInterval) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    if (numNewMessages) {
      Serial.println("Telegram: Got raw response");
      handleNewMessages(numNewMessages);
      // Removed recursive loop to prevent blocking main loop
    }
    lastTelegramTime = millis();

    flushNotifications();
  }
}
