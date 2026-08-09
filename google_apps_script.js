function doGet(e) {
    // Check if parameters exist
    if (!e || !e.parameter) {
        return ContentService.createTextOutput("Error: No parameters found");
    }

    var params = e.parameter;
    var date = params.date;
    var time = params.time;
    var grid = params.grid;
    var solar = params.solar;
    var price = params.price;
    var mode = params.mode;      // 0 = Solar, 1 = Balanceo
    var beny_w = params.beny_w;  // Charger power (W)
    var amps = params.amps;      // DLB current setpoint (A)

    // Validate critical fields
    if (!date || !time) {
        return ContentService.createTextOutput("Error: Missing Date or Time");
    }

    // Open the spreadsheet (Active Sheet)
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();

    // Append Row: [Date, Time, Grid, Solar, Price, Mode, Beny Power, Amps]
    sheet.appendRow([date, time, grid, solar, price, mode, beny_w, amps]);

    return ContentService.createTextOutput("Success");
}
