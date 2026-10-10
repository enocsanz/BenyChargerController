// Google Apps Script for the Beny DLB controller (M5StampS3).
//
//  - doGet  ?date=...&time=...   hourly row in the first sheet (unchanged)
//  - doPost {samples, events}    diagnostic batch -> "Muestras" / "Eventos"
//  - doGet  ?export=Muestras&key=...&rows=N   last N rows as CSV
//
//  - checkWatchdog (timer)       Telegram alert when the controller goes silent
//
// After pasting it: Deploy > Manage deployments > edit > Version: New version.
// The URL stays the same, so the firmware does not need to change.
//
// Watchdog, once: Project Settings > Script properties: TELEGRAM_TOKEN and
// TELEGRAM_CHAT (BOT_TOKEN and CHAT_ID from config.h). Then run setupWatchdog
// from the editor (it asks for permissions) and, to check, testTelegram.

// Key for the CSV export. Change it: anyone with the script URL and this key
// can read the data. While it is left as is, the export is disabled.
var EXPORT_KEY = 'CAMBIA-ESTA-CLAVE';

var SAMPLE_HEADERS = [
  'Fecha', 'Hora', 'Red W', 'Red min W', 'Red max W', 'Solar W', 'Precio E/kWh',
  'Modo (0 Solar, 1 Balanceo)', 'Beny W', 'Beny estado', 'Amps objetivo', 'Amps reales',
  'Termo estado', 'Termo W', 'Termo rele', 'Piscina estado', 'Piscina W', 'Piscina rele',
  'Piscina h hoy', 'Sobrante medio W', 'Heap libre', 'WiFi RSSI', 'Uptime min', 'Temp chip C', 'Caseta C', 'Caseta %HR', 'Precio excedentes E/kWh', 'Agua termo C', 'Agua medio C'
];
var EVENT_HEADERS = ['Fecha', 'Hora', 'Tipo', 'Detalle'];

function doGet(e) {
  if (!e || !e.parameter) {
    return ContentService.createTextOutput("Error: No parameters found");
  }
  var params = e.parameter;
  if (params.export) return exportCsv(params);

  var date = params.date;
  var time = params.time;
  var grid = params.grid;
  var solar = params.solar;
  var price = params.price;
  var mode = params.mode;      // 0 = Solar, 1 = Balanceo
  var beny_w = params.beny_w;  // Charger power (W)
  var amps = params.amps;      // DLB current setpoint (A)

  if (!date || !time) {
    return ContentService.createTextOutput("Error: Missing Date or Time");
  }

  // The hourly log keeps going to the first sheet, as before
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheets()[0];
  sheet.appendRow([date, time, grid, solar, price, mode, beny_w, amps]);
  return ContentService.createTextOutput("Success");
}

function doPost(e) {
  var lock = LockService.getScriptLock();
  lock.waitLock(20000); // batches must not interleave
  try {
    var data = JSON.parse(e.postData.contents);
    // For the watchdog: every batch, even an empty one, is a sign of life
    PropertiesService.getScriptProperties().setProperty('lastPost', String(Date.now()));
    var ss = SpreadsheetApp.getActiveSpreadsheet();
    appendRows(sheetFor(ss, 'Muestras', SAMPLE_HEADERS), data.samples || [], 2); // date, time
    appendRows(sheetFor(ss, 'Eventos', EVENT_HEADERS), data.events || [], 4);    // all columns
    return ContentService.createTextOutput("OK");
  } finally {
    lock.releaseLock();
  }
}

// The sheet, created at the end with a frozen header row if it is missing
function sheetFor(ss, name, headers) {
  var sheet = ss.getSheetByName(name);
  if (!sheet) {
    sheet = ss.insertSheet(name, ss.getNumSheets());
    sheet.appendRow(headers);
    sheet.setFrozenRows(1);
    sheet.getRange(1, 1, 1, headers.length).setFontWeight('bold');
  } else {
    // Columns added in later firmware versions: complete the header row
    var current = sheet.getRange(1, 1, 1, headers.length).getValues()[0];
    for (var i = 0; i < headers.length; i++) {
      if (current[i] === '' || current[i] === null) {
        sheet.getRange(1, i + 1).setValue(headers[i]).setFontWeight('bold');
      }
    }
  }
  return sheet;
}

// Rows already in the last DEDUP_ROWS are skipped: when the device does not
// get the answer in time it resends the whole batch, which had been written.
var DEDUP_ROWS = 300;

// keyCols: leading text columns that identify a row (numbers are left out:
// the sheet shows them with a decimal comma and they would never match).
function appendRows(sheet, rows, keyCols) {
  if (!rows.length) return;
  var width = rows[0].length;
  var last = sheet.getLastRow();
  if (last > 1) {
    var n = Math.min(DEDUP_ROWS, last - 1);
    var seen = {};
    sheet.getRange(last - n + 1, 1, n, keyCols).getDisplayValues().forEach(function (r) {
      seen[r.join('|')] = true;
    });
    rows = rows.filter(function (r) {
      return !seen[r.slice(0, keyCols).map(String).join('|')];
    });
    if (!rows.length) return;
  }
  var start = last + 1;
  // Dates and times stay as text: dd/mm/yyyy would otherwise be read with the
  // spreadsheet's locale and may swap day and month.
  sheet.getRange(start, 1, rows.length, 2).setNumberFormat('@');
  sheet.getRange(start, 1, rows.length, width).setValues(rows);
}

function exportCsv(params) {
  if (EXPORT_KEY === 'CAMBIA-ESTA-CLAVE' || params.key !== EXPORT_KEY) {
    return ContentService.createTextOutput("Error: export disabled or wrong key");
  }
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(params.export);
  if (!sheet) return ContentService.createTextOutput("Error: no sheet " + params.export);

  var last = sheet.getLastRow();
  var n = Math.min(parseInt(params.rows || '20000', 10), last - 1);
  // Raw values, not display ones: numbers keep a decimal point whatever the
  // spreadsheet's locale (dates and times are stored as text anyway)
  var header = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getValues();
  var body = n > 0 ? sheet.getRange(last - n + 1, 1, n, sheet.getLastColumn()).getValues() : [];
  var csv = header.concat(body).map(function (row) {
    return row.map(function (v) {
      v = String(v);
      return /[",\n]/.test(v) ? '"' + v.replace(/"/g, '""') + '"' : v;
    }).join(',');
  }).join('\n');
  return ContentService.createTextOutput(csv).setMimeType(ContentService.MimeType.CSV);
}

// --- Watchdog -------------------------------------------------------------
// The controller sends a batch every 5 min. If nothing arrives for WATCHDOG_MIN,
// it is down (power, WiFi, hang): nothing controls the car, the heater or the
// pool any more, so the user is told on Telegram, and again when it is back.
// The relays protect themselves meanwhile with their own countdown.
var WATCHDOG_MIN = 20;

function checkWatchdog() {
  var props = PropertiesService.getScriptProperties();
  var last = parseInt(props.getProperty('lastPost') || '0', 10);
  if (!last) return; // nothing received yet
  var silentMin = (Date.now() - last) / 60000;
  var alerted = props.getProperty('wdAlerted') === '1';
  var when = Utilities.formatDate(new Date(last), 'Europe/Madrid', 'dd/MM HH:mm');

  if (!alerted && silentMin > WATCHDOG_MIN) {
    if (sendTelegram('⚠️ El controlador no envia datos desde las ' + when + ' (hace ' +
                     Math.round(silentMin) + ' min). Puede estar sin alimentacion o sin ' +
                     'WiFi: nadie controla el coche. El termo y la depuradora vuelven a su ' +
                     'estado seguro solos en 15 min.')) {
      props.setProperty('wdAlerted', '1');
      props.setProperty('wdSince', String(last));
    }
  } else if (alerted && silentMin <= WATCHDOG_MIN) {
    var since = parseInt(props.getProperty('wdSince') || String(last), 10);
    var downMin = Math.round((last - since) / 60000);
    if (sendTelegram('✅ El controlador vuelve a enviar datos (unos ' + downMin + ' min sin senal).')) {
      props.setProperty('wdAlerted', '0');
    }
  }
}

function sendTelegram(text) {
  var props = PropertiesService.getScriptProperties();
  var token = props.getProperty('TELEGRAM_TOKEN');
  var chat = props.getProperty('TELEGRAM_CHAT');
  if (!token || !chat) {
    console.log('Watchdog: missing TELEGRAM_TOKEN / TELEGRAM_CHAT in Script properties');
    return false;
  }
  var r = UrlFetchApp.fetch('https://api.telegram.org/bot' + token + '/sendMessage', {
    method: 'post', payload: { chat_id: chat, text: text }, muteHttpExceptions: true
  });
  console.log('Telegram: ' + r.getResponseCode() + ' ' + r.getContentText().substring(0, 200));
  return r.getResponseCode() === 200;
}

// Run once from the editor: installs the 5-minute timer (replacing any old one)
function setupWatchdog() {
  ScriptApp.getProjectTriggers().forEach(function (t) {
    if (t.getHandlerFunction() === 'checkWatchdog') ScriptApp.deleteTrigger(t);
  });
  ScriptApp.newTrigger('checkWatchdog').timeBased().everyMinutes(5).create();
  console.log('Watchdog installed: every 5 min, alert after ' + WATCHDOG_MIN + ' min of silence');
}

// Run from the editor to check the Telegram settings
function testTelegram() {
  sendTelegram('🔔 Prueba del vigilante del controlador: los avisos llegaran a este chat.');
}
