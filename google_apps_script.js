// Google Apps Script for the Beny DLB controller (M5Dial).
//
//  - doGet  ?date=...&time=...   hourly row in the first sheet (unchanged)
//  - doPost {samples, events}    diagnostic batch -> "Muestras" / "Eventos"
//  - doGet  ?export=Muestras&key=...&rows=N   last N rows as CSV
//
// After pasting it: Deploy > Manage deployments > edit > Version: New version.
// The URL stays the same, so the firmware does not need to change.

// Key for the CSV export. Change it: anyone with the script URL and this key
// can read the data. While it is left as is, the export is disabled.
var EXPORT_KEY = 'CAMBIA-ESTA-CLAVE';

var SAMPLE_HEADERS = [
  'Fecha', 'Hora', 'Red W', 'Red min W', 'Red max W', 'Solar W', 'Precio E/kWh',
  'Modo (0 Solar, 1 Balanceo)', 'Beny W', 'Beny estado', 'Amps objetivo', 'Amps reales',
  'Termo estado', 'Termo W', 'Termo rele', 'Piscina estado', 'Piscina W', 'Piscina rele',
  'Piscina h hoy', 'Sobrante medio W', 'Heap libre', 'WiFi RSSI', 'Uptime min'
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
    var ss = SpreadsheetApp.getActiveSpreadsheet();
    appendRows(sheetFor(ss, 'Muestras', SAMPLE_HEADERS), data.samples || []);
    appendRows(sheetFor(ss, 'Eventos', EVENT_HEADERS), data.events || []);
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
  }
  return sheet;
}

function appendRows(sheet, rows) {
  if (!rows.length) return;
  var width = rows[0].length;
  var start = sheet.getLastRow() + 1;
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
  var header = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getDisplayValues();
  var body = n > 0 ? sheet.getRange(last - n + 1, 1, n, sheet.getLastColumn()).getDisplayValues() : [];
  var csv = header.concat(body).map(function (row) {
    return row.map(function (v) {
      v = String(v);
      return /[",\n]/.test(v) ? '"' + v.replace(/"/g, '""') + '"' : v;
    }).join(',');
  }).join('\n');
  return ContentService.createTextOutput(csv).setMimeType(ContentService.MimeType.CSV);
}
