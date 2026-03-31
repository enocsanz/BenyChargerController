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
    var tuya = params.tuya;
    var price = params.price;

    // Validate critical fields
    if (!date || !time) {
        return ContentService.createTextOutput("Error: Missing Date or Time");
    }

    // Open the spreadsheet (Active Sheet)
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();

    // Append Row: [Date, Time, Grid Power, Solar Power, Tuya Power, Price]
    sheet.appendRow([date, time, grid, solar, tuya, price]);

    return ContentService.createTextOutput("Success");
}
