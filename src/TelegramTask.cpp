#include "TelegramTask.h"
#include "BenyTask.h"

// #include "WeatherTask.h" REMOVED

#include "config.h"
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>

// Externs from main.cpp
// extern float demand_kwh; REMOVED
// extern bool charging_schedule[24]; REMOVED
extern float getCurrentPrice();
extern int32_t current_grid_power;
extern int32_t current_pv_power;
extern int charging_mode;       // 0=Solar, 1=Balanceo, 2=Turbo
extern int max_grid_power;      // Added for dynamic limit
extern void saveMode(int mode); // Added for persistence
extern void saveMaxGridPower(int watts);
extern void saveConfigVals();
extern unsigned long pause_time_ms;
extern unsigned long resume_time_ms;
extern int resume_margin_watts;
extern bool auto_paused;
// extern float tuya_current_power;
// extern bool is_planned_for_tomorrow; REMOVED

WiFiClientSecure clientTCP;
UniversalTelegramBot bot(BOT_TOKEN, clientTCP);

unsigned long lastTelegramTime = 0;
const int telegramInterval = 2000;

void setupTelegram() {
  clientTCP.setInsecure();
  clientTCP.setHandshakeTimeout(20000);
  // bot.longPoll = 1; // Faster response?
}

// Duplicate function definition removed.

String last_chat_id = ""; // Store for proactive notifications

void handleNewMessages(int numNewMessages) {
  Serial.println("Telegram: Handling messages...");

  for (int i = 0; i < numNewMessages; i++) {
    // Save Chat ID for notifications
    String chat_id = String(bot.messages[i].chat_id);
    last_chat_id = chat_id;

    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    if (text == "/start" || text == "/help") {
      String msg = "Control Beny V2\n\n";
      msg += "📊 /status - Ver potencias y modo actual\n\n";
      msg += "MODOS DE CARGA:\n";
      msg += "🔋 SOLAR: /solar - Carga con excedentes. Si no hay sol, mantiene un minimo de 6A. Se detiene si superas los 4.6kW.\n";
      msg += "⚖️ BALANCEO: /balanceo - Carga dinamica hasta aprovechar 4.6kW de casa.\n";
      msg += "🛑 OFF: /off - Apagar el cargador por completo.\n\n";
      msg += "AJUSTES: \n";
      msg += "Limite Red: /set_limit W\n";
      msg += "Tiempo Pausa: /set_pausa seg\n";
      msg += "Tiempo Reinicio: /set_reinicio seg\n";
      msg += "Margen: /set_margen W\n";
      bot.sendMessage(chat_id, msg, "");
    } else if (text == "/status") {
      String msg = "📊 ESTADO DEL SISTEMA \n\n";

      msg += "💰 Precio Luz (PVPC): " + String(getCurrentPrice(), 3) + " E/kWh\n\n";

      msg += "🏠 Red (Grid): " + String(current_grid_power > 0 ? "+" : "") + String((float)current_grid_power, 0) +
             " W / " + String(max_grid_power) + " W\n";
      msg += "   Limites: Pausa " + String(pause_time_ms/1000) + "s, Reinicio " + String(resume_time_ms/1000) + "s (" + String(resume_margin_watts) + "W)\n";
      msg += "   (+ Importando / - Exportando)\n\n";
      msg += "☀️ Solar: " + String(current_pv_power) + " W\n\n";

      BenyData bd = getBenyData();
      msg += "🔌 Cargador Beny: " + String(bd.power, 1) + " W\n";
      msg += "   STATUS: " + bd.status + "\n\n";

      msg += "🚀 MODO ACTIVO: ";
      if (charging_mode == 0) {
        msg += "SOLAR\n";
        msg += "   Solo Excedentes (min 6A)";
      } else if (charging_mode == 1) {
        msg += "BALANCEO\n";
        msg += "   Carga Dinamica (Max Red " + String(max_grid_power) + "W)";
      } else {
        msg += "OFF\n";
        msg += "   Cargador Desactivado";
      }
      msg += "\n";
      
      if (auto_paused) {
        msg += "⚠️ ESTADO: PAUSADO POR EXCESO DE RED\n";
      }

      bot.sendMessage(chat_id, msg, "");

    } else if (text.startsWith("/set_price ")) {
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
    } else if (text.startsWith("/set_limit ")) {
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
    } else if (text.startsWith("/set_pausa ")) {
      unsigned long val = text.substring(11).toInt();
      if (val >= 10 && val <= 3600) {
        pause_time_ms = val * 1000;
        saveConfigVals();
        bot.sendMessage(chat_id, "Tolerancia de pausa puesta a " + String(val) + "s.", "");
      }
    } else if (text.startsWith("/set_reinicio ")) {
      unsigned long val = text.substring(14).toInt();
      if (val >= 10 && val <= 3600) {
        resume_time_ms = val * 1000;
        saveConfigVals();
        bot.sendMessage(chat_id, "Espera de reinicio puesta a " + String(val) + "s.", "");
      }
    } else if (text.startsWith("/set_margen ")) {
      int val = text.substring(12).toInt();
      if (val >= 100 && val <= 8000) {
        resume_margin_watts = val;
        saveConfigVals();
        bot.sendMessage(chat_id, "Margen de reinicio puesto a " + String(val) + "W.", "");
      }
    } else if (text == "/turbo") {
      bot.sendMessage(chat_id, "❌ El modo Turbo ha sido eliminado.", "");
    } else if (text == "/solar") {
      charging_mode = 0;
      saveMode(charging_mode);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Modo: SOLAR (Solo Excedentes).", "");
    } else if (text == "/balanceo") {
      charging_mode = 1;
      saveMode(charging_mode);
      manual_logic_trigger = true;
      bot.sendMessage(
          chat_id, "Modo: BALANCEO (Lim Red " + String(max_grid_power) + "W).", "");
    } else if (text == "/off" || text == "/stop") {
      charging_mode = 3;
      saveMode(charging_mode);
      manual_logic_trigger = true;
      bot.sendMessage(chat_id, "Modo: OFF. Cargador desactivado.", "");
    }
  }
}

void sendTelegramNotification(String msg) {
  if (last_chat_id != "") {
    bot.sendMessage(last_chat_id, msg, "");
  } else if (String(CHAT_ID) != "") {
    bot.sendMessage(CHAT_ID, msg, "");
  } else {
    Serial.println("Tele: No ID to notify.");
  }
}

void loopTelegram() {
  // Debug
  if (millis() % 10000 == 0) {
    // Very verbose, maybe tone down
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
  }
}
