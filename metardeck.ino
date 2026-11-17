#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <time.h>
#include <math.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h> 
#include <Fonts/FreeSans9pt7b.h>

// --- USER CONFIG ---
String mySSID = "";
String myPASS = "";

String airports[10]; 
String tafFallbacks[10]; 
String runways[10]; 
float aptLat[10] = {0};
float aptLon[10] = {0};
const int numAirports = 10; 
int currentIdx = 0;
int weatherMode = 0; // 0=METAR, 1=TAF, 2=WINDS

bool inConfigMode = false;
bool inWifiSetup = false;
int lastFetchSlot = -1; 
bool invertedColors = false; 
unsigned long lastFetchTime = 0;
unsigned long lastInteraction = 0; 

// --- BATTERY TRACKING ---
int fetchCount = 0;
const int MAX_FETCHES = 1800; 

// --- CAPTIVE PORTAL ---
const byte DNS_PORT = 53;
DNSServer dnsServer;
String scanResultsHTML = "";

// Dynamic Color Macros
#define BG_COLOR (invertedColors ? GxEPD_WHITE : GxEPD_BLACK)
#define FG_COLOR (invertedColors ? GxEPD_BLACK : GxEPD_WHITE)

WebServer server(80);
Preferences prefs;

// --- SCHEMATIC PINS ---
#define EPD_CS      45
#define EPD_DC      46
#define EPD_RST     47
#define EPD_BUSY    48
#define EPD_PWR_EN  7   
#define SPI_SCK     12
#define SPI_MOSI    11
#define SPI_MISO    -1  

#define BTN_TOP       2   
#define BTN_BOTTOM    1   
#define ROCKER_UP     6   
#define ROCKER_DOWN   4   
#define ROCKER_IN     5   

GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT> display(GxEPD2_579_GDEY0579T93(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// Forward Declarations
void loadPreferences();
void enterConfigMode();
void startCaptivePortal();
void handleRoot();
void handleSave();
void handleResetBatt();
void handleWifiRoot();
void handleWifiSave();
void fetchAndRender(String icao);
void fetchMETAR(String icao);
void fetchTAF(String icao);
void fetchWinds(String icao);
void drawAirportSelector(String icao);
void drawWeatherIcon(int x, int y, String raw, bool isNight);
void drawEinkSlash(int px, int py, int len, int ang); 
void drawWindsock(int px, int py, int wdir, int wspd);
void drawCompass(int cx, int cy, int r, int wdir, int wspd, int wgst);
void drawTFRIndicator(int cx, int cy);
void drawWaterDrop(int x, int y, int r); 
void drawBattery(int x, int y);
void drawErrorScreen(String icao, String type, int httpCode);
void printWithDegree(int x, int y, const char* label, float val);
void printWindDecoded(int x, int y, int dir, int spd);
String formatFAAWind(int dir, int spd, float temp);
String expandCover(String c);
String expandWx(String w);
String getBestRunway(String icao, int wdir, int aptIdx); 
String formatAlt(int feet);

void setup() {
  Serial.begin(115200);
  pinMode(EPD_PWR_EN, OUTPUT); 
  pinMode(BTN_TOP, INPUT_PULLUP); pinMode(BTN_BOTTOM, INPUT_PULLUP);
  pinMode(ROCKER_UP, INPUT_PULLUP); pinMode(ROCKER_DOWN, INPUT_PULLUP); pinMode(ROCKER_IN, INPUT_PULLUP);

  loadPreferences(); 
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);

  digitalWrite(EPD_PWR_EN, HIGH);
  delay(100); 
  display.init(115200);
  delay(100); 

  if (mySSID == "YOUR_WIFI_NAME" || mySSID == "") {
    startCaptivePortal();
    return; 
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_15dBm); 
  WiFi.begin(mySSID.c_str(), myPASS.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
    delay(500); 
    attempts++; 
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawErrorScreen(airports[currentIdx], "Boot Wi-Fi", -1);
  } else {
    configTzTime("PST8PDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org");
    struct tm ti;
    int ntpTries = 0;
    while(!getLocalTime(&ti) && ntpTries < 20){ delay(500); ntpTries++; }
    
    fetchAndRender(airports[currentIdx]);
    
    if (getLocalTime(&ti)) {
      int win = (ti.tm_min >= 32) ? 32 : 2;
      lastFetchSlot = ti.tm_hour * 60 + win;
    }
  }
  
  lastInteraction = millis(); 

  gpio_wakeup_enable((gpio_num_t)BTN_TOP, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BTN_BOTTOM, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)ROCKER_UP, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)ROCKER_DOWN, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)ROCKER_IN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}

void loop() {
  if (inWifiSetup) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(10);
    return;
  }

  if (inConfigMode) {
    server.handleClient();
    delay(10);
    return; 
  }

  struct tm ti;
  if (getLocalTime(&ti)) {
    bool inWindow = (ti.tm_min >= 2 && ti.tm_min <= 7) || (ti.tm_min >= 32 && ti.tm_min <= 37);
    if (inWindow) {
      int win = (ti.tm_min >= 32) ? 32 : 2;
      int currentSlot = ti.tm_hour * 60 + win;
      if (currentSlot != lastFetchSlot) {
        fetchAndRender(airports[currentIdx]);
        lastFetchSlot = currentSlot;
      }
    }
  }

  if (digitalRead(ROCKER_UP) == LOW) {
    lastInteraction = millis();
    currentIdx = (currentIdx + 1) % numAirports;
    if(airports[currentIdx] == "") currentIdx = 0; 
    drawAirportSelector(airports[currentIdx]); 
    delay(300);
  }
  
  if (digitalRead(ROCKER_DOWN) == LOW) {
    lastInteraction = millis();
    currentIdx = (currentIdx - 1 + numAirports) % numAirports;
    while(airports[currentIdx] == "" && currentIdx > 0) currentIdx--; 
    drawAirportSelector(airports[currentIdx]); 
    delay(300);
  }

  if (digitalRead(BTN_BOTTOM) == LOW) {
    lastInteraction = millis();
    unsigned long pressStart = millis();
    int triggerLevel = 0; 

    while (digitalRead(BTN_BOTTOM) == LOW) {
      unsigned long heldTime = millis() - pressStart;

      if (heldTime >= 6000 && triggerLevel == 1) {
        triggerLevel = 2;
        display.setPartialWindow(20, 20, 300, 50); display.firstPage();
        do {
          display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
          display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 50);
          display.print("BATTERY RESET");
        } while (display.nextPage());
        break; 
      }
      else if (heldTime >= 3000 && triggerLevel == 0) {
        triggerLevel = 1;
        display.setPartialWindow(20, 20, 300, 50); display.firstPage();
        do {
          display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
          display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 50);
          display.print("THEME SWAP");
        } while (display.nextPage());
      }
      delay(10);
    }

    if (triggerLevel == 2) {
      fetchCount = 0;
      prefs.begin("mfd", false);
      prefs.putInt("batt", 0);
      prefs.end();
    } else if (triggerLevel == 1) {
      invertedColors = !invertedColors;
      prefs.begin("mfd", false);
      prefs.putBool("inverted", invertedColors);
      prefs.end();
    } else {
      weatherMode = (weatherMode + 1) % 3;
      display.setPartialWindow(20, 20, 300, 50); display.firstPage();
      do {
        display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
        display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 50);
        if (weatherMode == 0) display.print("METAR MODE");
        else if (weatherMode == 1) display.print("TAF MODE");
        else display.print("WINDS ALOFT");
      } while (display.nextPage());
      delay(600);
    }

    while(digitalRead(BTN_BOTTOM) == LOW) delay(10); 
    fetchAndRender(airports[currentIdx]);
  }

  if (digitalRead(ROCKER_IN) == LOW) {
    lastInteraction = millis();
    fetchAndRender(airports[currentIdx]);
    while(digitalRead(ROCKER_IN) == LOW) delay(10); 
  }

  if (digitalRead(BTN_TOP) == LOW) {
    lastInteraction = millis();
    unsigned long pressStart = millis();
    int triggerLevel = 0; 

    while (digitalRead(BTN_TOP) == LOW) {
      unsigned long heldTime = millis() - pressStart;

      if (heldTime >= 6000 && triggerLevel == 1) {
        triggerLevel = 2;
        display.setPartialWindow(20, 20, 300, 50); display.firstPage();
        do {
          display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
          display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 50);
          display.print("WIFI SETUP");
        } while (display.nextPage());
        break; 
      }
      else if (heldTime >= 3000 && triggerLevel == 0) {
        triggerLevel = 1;
        display.setPartialWindow(20, 20, 300, 50); display.firstPage();
        do {
          display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
          display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 50);
          display.print("AIRPORT CONFIG");
        } while (display.nextPage());
      }
      delay(10);
    }

    if (triggerLevel == 2) {
      startCaptivePortal();
    } else if (triggerLevel == 1) {
      enterConfigMode();
    } else {
      currentIdx = 0;
      weatherMode = 0;
      fetchAndRender(airports[currentIdx]);
    }
    while(digitalRead(BTN_TOP) == LOW) delay(10);
  }

  if (millis() - lastInteraction > 15000 && !inConfigMode && !inWifiSetup) {
    display.powerOff();
    digitalWrite(EPD_PWR_EN, LOW);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    uint64_t sleep_us = 60 * 1000000ULL; 
    if (getLocalTime(&ti)) {
      int current_min = ti.tm_min;
      int current_sec = ti.tm_sec;
      int seconds_to_next;

      if (current_min < 2) {
        seconds_to_next = ((1 - current_min) * 60) + (60 - current_sec);
      } else if (current_min < 32) {
        seconds_to_next = ((31 - current_min) * 60) + (60 - current_sec);
      } else {
        seconds_to_next = ((61 - current_min) * 60) + (60 - current_sec);
      }
      sleep_us = (uint64_t)(seconds_to_next + 2) * 1000000ULL; 
    }

    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_light_sleep_start();

    delay(100); 
    digitalWrite(EPD_PWR_EN, HIGH); 
    delay(100); 
    display.init(115200);

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
      lastInteraction = millis(); 
    } else {
      lastInteraction = millis() - 10000; 
    }
  }
}

void loadPreferences() {
  prefs.begin("mfd", false);
  
  mySSID = prefs.getString("wifi_ssid", "YOUR_WIFI_NAME");
  myPASS = prefs.getString("wifi_pass", "YOUR_WIFI_PASSWORD");

  String defApts[] = {"KLVK", "KTCY", "KSCK", "KMOD", "KCCR", "KRHV", "KC83", "KSJC", "KOAK", "KPAO"};
  String defTafs[] = {"", "KSCK", "", "", "", "KSJC", "KLVK", "", "", "KSJC"}; 
  String defRwys[] = {"25R/25L, 07L/07R", "26, 08", "29R/29L, 11L/11R", "28R/28L, 10R/10L", "19R/19L, 01R/01L", "31R/31L, 13R/13L", "30, 12", "30L/30R, 12R/12L", "30, 12", "31, 13"};
  float defLats[]  = {37.6934, 37.6890, 37.8942, 37.6258, 37.9897, 37.3329, 37.8284, 37.3626, 37.7213, 37.4611};
  float defLons[]  = {-121.8204, -121.4418, -121.2383, -120.9544, -122.0569, -121.8195, -121.6258, -121.9290, -122.2207, -122.1150};

  for (int i=0; i<numAirports; i++) {
    String keyApt = "apt" + String(i);
    String keyTaf = "taf" + String(i);
    String keyRwy = "rwy" + String(i);
    String keyLat = "lat" + String(i);
    String keyLon = "lon" + String(i);
    airports[i] = prefs.getString(keyApt.c_str(), defApts[i]);
    tafFallbacks[i] = prefs.getString(keyTaf.c_str(), defTafs[i]);
    runways[i] = prefs.getString(keyRwy.c_str(), defRwys[i]);
    aptLat[i] = prefs.getFloat(keyLat.c_str(), defLats[i]);
    aptLon[i] = prefs.getFloat(keyLon.c_str(), defLons[i]);
  }
  
  invertedColors = prefs.getBool("inverted", false); 
  fetchCount = prefs.getInt("batt", 0);
  prefs.end();
}

void startCaptivePortal() {
  inWifiSetup = true;
  
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(20, 60); display.print("WIFI SETUP MODE");
    display.drawLine(20, 80, 770, 80, FG_COLOR);
    display.setFont(&FreeSans12pt7b);
    display.setCursor(20, 140); display.print("1. On your phone, connect to Wi-Fi:");
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(20, 190); display.print("MFD SETUP");
    display.setFont(&FreeSans12pt7b);
    display.setCursor(20, 260); display.print("2. A login screen should appear automatically.");
    display.setCursor(20, 300); display.print("   (Or open browser to 192.168.4.1)");
  } while (display.nextPage());

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("MFD SETUP");

  int n = WiFi.scanNetworks();
  scanResultsHTML = "<select name='ssid' class='dropdown'>";
  if (n == 0) {
    scanResultsHTML += "<option value=''>No 2.4GHz networks found</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      scanResultsHTML += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
    }
  }
  scanResultsHTML += "</select>";

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleWifiRoot);
  server.on("/save_wifi", HTTP_POST, handleWifiSave);
  
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  
  server.begin();
}

void handleWifiRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body{font-family:sans-serif; margin:20px; background:#f4f4f4;}";
  html += "h2{color:#333; margin-top:0;}";
  html += ".dropdown, input[type=password]{width:100%; padding:12px; margin-bottom:20px; font-size:16px; border:1px solid #ccc; border-radius:5px;}";
  html += ".btn{width:100%; padding:15px; background:#007BFF; color:white; font-size:18px; border:none; border-radius:5px; cursor:pointer;}";
  html += "form{background:white; padding:20px; border-radius:10px; max-width:400px; margin:auto; box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
  html += "</style></head><body>";
  html += "<form action='/save_wifi' method='POST'>";
  html += "<h2>MFD Wi-Fi Setup</h2>";
  html += "<p style='color:#666; font-size:14px;'>Only 2.4GHz networks are supported.</p>";
  html += "<label><b>Select Network</b></label><br>";
  html += scanResultsHTML;
  html += "<label><b>Password</b></label><br>";
  html += "<input type='password' name='pass' placeholder='Leave blank if open network'>";
  html += "<input type='submit' class='btn' value='Connect & Reboot'>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleWifiSave() {
  if (server.hasArg("ssid")) {
    prefs.begin("mfd", false);
    prefs.putString("wifi_ssid", server.arg("ssid"));
    prefs.putString("wifi_pass", server.arg("pass"));
    prefs.end();
    server.send(200, "text/html", "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head><body style='font-family:sans-serif; padding:20px; text-align:center;'><h2>Credentials Saved!</h2><p>Rebooting flight deck. You can close this window.</p></body></html>");
    delay(2000);
    ESP.restart();
  }
}

void enterConfigMode() {
  inConfigMode = true;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA); 
    WiFi.setTxPower(WIFI_POWER_15dBm); 
    WiFi.begin(mySSID.c_str(), myPASS.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
      delay(500);
      attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
      startCaptivePortal();
      return; 
    }
  }

  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(20, 100); display.print("AIRPORT CONFIG ACTIVE");
    display.drawLine(20, 120, 770, 120, FG_COLOR);
    display.setFont(&FreeSans12pt7b);
    display.setCursor(20, 180); display.print("Connect to same Wi-Fi network and open browser to:");
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(20, 240); display.print("http://" + WiFi.localIP().toString());
  } while (display.nextPage());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset_batt", HTTP_POST, handleResetBatt);
  server.begin();
}

void handleRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body{font-family:sans-serif; margin:15px; background:#f4f4f4;}";
  html += "input[type=text]{padding:10px; font-size:16px; border:1px solid #ccc; border-radius:5px; box-sizing:border-box;}";
  html += ".btn{width:100%; padding:15px; background:#007BFF; color:white; font-size:18px; border:none; border-radius:5px; margin-bottom:20px;}";
  html += ".btn-green{background:#28a745;}";
  html += "form{background:white; padding:20px; border-radius:10px; max-width:600px; margin:auto; margin-bottom:20px;}";
  html += "label{font-weight:bold; color:#333; font-size:14px; margin-bottom:5px; display:block;}";
  html += "</style></head><body>";
  
  html += "<form action='/save' method='POST'><h2>MFD Databases</h2>";
  for (int i=0; i<numAirports; i++) {
    html += "<label>Slot " + String(i+1) + " (ICAO, TAF Ref, Runways)</label>";
    html += "<div style='display:flex; gap:10px; margin-bottom:15px;'>";
    html += "<input type='text' name='apt" + String(i) + "' value='" + airports[i] + "' maxlength='4' style='width:25%;' placeholder='ICAO'>";
    html += "<input type='text' name='taf" + String(i) + "' value='" + tafFallbacks[i] + "' maxlength='4' style='width:25%;' placeholder='TAF Ref'>";
    html += "<input type='text' name='rwy" + String(i) + "' value='" + runways[i] + "' style='width:50%;' placeholder='Runways'>";
    html += "</div>";
  }
  html += "<input type='submit' class='btn' value='Save Config & Reboot'></form>";

  html += "<form action='/reset_batt' method='POST'>";
  html += "<h2>Battery Management</h2>";
  html += "<p>Cycles used: " + String(fetchCount) + " / " + String(MAX_FETCHES) + "</p>";
  html += "<input type='submit' class='btn btn-green' value='I Just Recharged the Battery'></form>";
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  prefs.begin("mfd", false);
  for (int i=0; i<numAirports; i++) {
    String keyApt = "apt" + String(i);
    String keyTaf = "taf" + String(i);
    String keyRwy = "rwy" + String(i);
    String keyLat = "lat" + String(i);
    String keyLon = "lon" + String(i);
    
    if (server.hasArg(keyApt)) {
      String valApt = server.arg(keyApt);
      valApt.toUpperCase(); valApt.trim();
      if (valApt != airports[i]) {
        prefs.putFloat(keyLat.c_str(), 0.0);
        prefs.putFloat(keyLon.c_str(), 0.0);
      }
      prefs.putString(keyApt.c_str(), valApt);
    }
    
    if (server.hasArg(keyTaf)) {
      String valTaf = server.arg(keyTaf);
      valTaf.toUpperCase(); valTaf.trim();
      prefs.putString(keyTaf.c_str(), valTaf);
    }
    
    if (server.hasArg(keyRwy)) {
      String valRwy = server.arg(keyRwy);
      valRwy.toUpperCase(); valRwy.trim();
      prefs.putString(keyRwy.c_str(), valRwy);
    }
  }
  prefs.end();
  server.send(200, "text/html", "<html><body style='font-family:sans-serif; text-align:center; padding:20px;'><h2>Saved! Rebooting MFD...</h2><p>You may close this page.</p></body></html>");
  delay(1000); ESP.restart(); 
}

void handleResetBatt() {
  fetchCount = 0;
  prefs.begin("mfd", false);
  prefs.putInt("batt", 0);
  prefs.end();
  server.send(200, "text/html", "<html><body style='font-family:sans-serif; text-align:center; padding:20px;'><h2>Tracker Reset! Rebooting MFD...</h2></body></html>");
  delay(1000); ESP.restart(); 
}

void drawErrorScreen(String icao, String type, int httpCode) {
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
    
    drawBattery(761, 2);

    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(20, 45); display.print(icao + " - CONNECTION ERROR");
    display.drawLine(20, 60, 770, 60, FG_COLOR);
    
    display.setFont(&FreeSans12pt7b);
    display.setCursor(20, 120); 
    display.print("Failed to fetch " + type + " data.");
    display.setCursor(20, 160);
    if (httpCode == -11) {
      display.print("Reason: Server Read Timeout (-11)");
    } else if (httpCode == 0) {
      display.print("Reason: Station returned empty report");
    } else if (httpCode < 0) {
      display.print("Reason: Wi-Fi or Socket Error (" + String(httpCode) + ")");
    } else {
      display.print("Reason: API returned HTTP " + String(httpCode));
    }
    display.setCursor(20, 220); 
    display.print("Will automatically retry on next cycle.");
  } while (display.nextPage());
}

void drawAirportSelector(String icao) {
  display.setPartialWindow(20, 15, 300, 50); display.firstPage();
  do {
    display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
    display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 45); display.print(icao + " >"); 
  } while (display.nextPage());
}

void fetchAndRender(String icao) {
  lastFetchTime = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    delay(100); 
    WiFi.mode(WIFI_STA); 
    WiFi.setTxPower(WIFI_POWER_15dBm); 
    WiFi.begin(mySSID.c_str(), myPASS.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
      delay(500);
      attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
      drawErrorScreen(icao, "Wi-Fi Connect", -1);
      return; 
    }
    
    configTzTime("PST8PDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org");
    delay(1500); 
  }

  fetchCount++;
  prefs.begin("mfd", false);
  prefs.putInt("batt", fetchCount);
  prefs.end();
  
  display.setPartialWindow(20, 15, 350, 50); display.firstPage();
  do {
    display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
    display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 45); display.print(icao + " (FETCHING...)"); 
  } while (display.nextPage());

  if (weatherMode == 0) fetchMETAR(icao);
  else if (weatherMode == 1) fetchTAF(icao);
  else fetchWinds(icao);
}

void fetchMETAR(String icao) {
  bool hasTFR = false;
  int httpCode = -1;
  String payloadMetar = "";

  String notamUrl = "https://aviationweather.gov/api/data/notam?ids=" + icao + "&format=json";
  for (int i = 0; i < 2; i++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient httpNotam;
    httpNotam.useHTTP10(true);
    if (httpNotam.begin(client, notamUrl)) {
      httpNotam.setConnectTimeout(5000);
      httpNotam.setTimeout(10000);
      httpNotam.setReuse(false);
      httpNotam.setUserAgent("MFD-FlightDeck/1.0");
      
      if (httpNotam.GET() == 200) {
        String payload = httpNotam.getString();
        if (payload.indexOf("\"TFR\"") != -1 || payload.indexOf("TEMPORARY FLIGHT RESTRICTION") != -1 || payload.indexOf("VIP") != -1) {
          hasTFR = true;
        }
        httpNotam.end();
        break;
      }
      httpNotam.end();
    }
    delay(500);
  }

  String url = "https://aviationweather.gov/api/data/metar?ids=" + icao + "&format=json";
  for (int i = 0; i < 3; i++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    if (http.begin(client, url)) {
      http.setConnectTimeout(5000);
      http.setTimeout(10000);
      http.setReuse(false);
      http.setUserAgent("MFD-FlightDeck/1.0");
      
      httpCode = http.GET();
      if (httpCode == 200) {
        payloadMetar = http.getString();
        http.end();
        break; 
      }
      Serial.printf("[METAR] Attempt %d failed: %d (%s)\n", i + 1, httpCode, http.errorToString(httpCode).c_str());
      http.end();
    }
    delay(1000); 
  }
  
  if (httpCode == 200) {
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, payloadMetar);
    if (err || doc.size() == 0) {
      drawErrorScreen(icao, "METAR", 0);
      return;
    }
    JsonObject obj = doc[0];

    if (!obj["lat"].isNull() && !obj["lon"].isNull()) {
      float newLat = obj["lat"].as<float>();
      float newLon = obj["lon"].as<float>();
      if (fabs(aptLat[currentIdx] - newLat) > 0.001 || fabs(aptLon[currentIdx] - newLon) > 0.001) {
        aptLat[currentIdx] = newLat;
        aptLon[currentIdx] = newLon;
        prefs.begin("mfd", false);
        prefs.putFloat(("lat" + String(currentIdx)).c_str(), newLat);
        prefs.putFloat(("lon" + String(currentIdx)).c_str(), newLon);
        prefs.end();
      }
    }

    const char* cat = obj["fltCat"] | "UNK";
    String vis = obj["visib"].isNull() ? "N/A" : obj["visib"].as<String>();
    const char* raw = obj["rawOb"] | "";
    String name = obj["name"] | "Unknown Station";
    float temp = obj["temp"] | 0.0;
    float dew = obj["dewp"] | 0.0;
    int wdir = obj["wdir"] | 0;
    int wspd = obj["wspd"] | 0;
    int wgst = obj["wgst"] | 0;
    float altim_inHg = (obj["altim"] | 1013.25) * 0.02953;
    float elev_ft = (obj["elev"] | 0.0) * 3.28084;

    display.setFullWindow(); display.firstPage();
    do {
      display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
      
      struct tm ti; getLocalTime(&ti);
      char tBuf[40]; strftime(tBuf, sizeof(tBuf), "%a, %b %d %H:%M %Z", &ti);
      display.setFont(&FreeSans9pt7b); display.setCursor(530, 20); display.print(tBuf);
      
      drawBattery(761, 2);

      display.setFont(&FreeSansBold18pt7b);
      display.setCursor(20, 45); display.print(icao);
      
      int16_t bx, by; uint16_t bw, bh;
      display.getTextBounds(icao, 0, 0, &bx, &by, &bw, &bh);
      int idWidth = bw;
      display.getTextBounds(cat, 0, 0, &bx, &by, &bw, &bh);
      int catWidth = bw;
      int boxX = 20 + idWidth + 12; 

      String catStr = String(cat);
      if (catStr == "MVFR" || catStr == "IFR" || catStr == "LIFR") {
        display.fillRect(boxX, 16, catWidth + 14, 35, FG_COLOR);
        display.setTextColor(BG_COLOR); 
      } else {
        display.drawRect(boxX, 16, catWidth + 14, 35, FG_COLOR);
      }

      display.setCursor(boxX + 7, 45); 
      display.print(cat);
      display.setTextColor(FG_COLOR); 
      
      display.setFont(&FreeSans12pt7b);
      int comma = name.lastIndexOf(", "); if(comma > 0) name = name.substring(0, comma);
      display.setCursor(boxX + catWidth + 14 + 15, 43); display.print(name);
      
      display.setFont(&FreeSans9pt7b); 
      display.setCursor(580, 43); display.printf("Elev: %.0f ft", elev_ft);

      display.drawLine(20, 60, 580, 60, FG_COLOR);

      display.setFont(&FreeSansBold12pt7b);
      display.setCursor(20, 105); display.printf("Vis: %s SM", vis.c_str());
      display.setCursor(220, 105); display.printf("Alt: %.2f inHg", altim_inHg);
      
      float pressure_alt = elev_ft + (29.92 - altim_inHg) * 1000.0;
      float isa_temp = 15.0 - (2.0 * (elev_ft / 1000.0));
      float density_alt = pressure_alt + 120.0 * (temp - isa_temp);
      display.setCursor(400, 105); display.printf("DA: %.0f ft", density_alt);

      printWithDegree(20, 150, "Temp", temp);
      printWithDegree(220, 150, "Dew", dew);
      
      float rh = 100.0 * (exp((17.625 * dew) / (243.04 + dew)) / exp((17.625 * temp) / (243.04 + temp)));
      char rhStr[20];
      sprintf(rhStr, "RH: %.0f%%", rh);
      display.setCursor(400, 150); 
      display.print(rhStr);
      display.getTextBounds(rhStr, 400, 150, &bx, &by, &bw, &bh);
      drawWaterDrop(400 + bw + 15, 142, 7); 
      
      String rawStr = String(raw);
      
      String iconStr = rawStr; 
      int rmkIdx = iconStr.indexOf(" RMK");
      if (rmkIdx > 0) {
        iconStr = iconStr.substring(0, rmkIdx); 
      }

      String cloudStr = "";
      JsonArray clouds = obj["clouds"];
      if (clouds.size() == 0) {
        if (rawStr.indexOf("CLR") >= 0 || rawStr.indexOf("SKC") >= 0) cloudStr = "Sky Clear < 12K ft";
        else cloudStr = expandCover(obj["cover"] | "UNK");
      } else {
        for (int i=0; i<clouds.size(); i++) {
          String cvr = expandCover(clouds[i]["cover"] | "UNK");
          int base = clouds[i]["base"] | 0;
          cloudStr += cvr;
          if (base > 0) { cloudStr += " " + formatAlt(base) + " ft  "; }
        }
      }
      display.setCursor(20, 195); display.print("Clouds: " + cloudStr);
      
      display.setCursor(20, 240); display.print("Active Rwy: " + getBestRunway(icao, wdir, currentIdx));

      int iconX = 460; 
      if (cloudStr.length() > 20) iconX = 550; 
      
      drawWeatherIcon(iconX, 200, iconStr, (ti.tm_hour >= 19 || ti.tm_hour < 6));
      
      drawCompass(660, 160, 65, wdir, wspd, wgst);
      drawWindsock(750, 40, wdir, wspd);
      
      if (hasTFR) drawTFRIndicator(660, 30);
      
      display.setFont(&FreeSans9pt7b);
      display.setCursor(20, 270); display.print(rawStr);
    } while (display.nextPage());
  } else {
    drawErrorScreen(icao, "METAR", httpCode);
  }
}

void fetchTAF(String icao) {
  int httpCode = -1;
  String payloadTaf = "";
  
  String target = tafFallbacks[currentIdx];
  if (target == "") target = icao; 

  String url = "https://aviationweather.gov/api/data/taf?ids=" + target + "&format=json";
  for (int i = 0; i < 3; i++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    if (http.begin(client, url)) {
      http.setConnectTimeout(5000);
      http.setTimeout(10000);
      http.setReuse(false);
      http.setUserAgent("MFD-FlightDeck/1.0");
      
      httpCode = http.GET();
      if (httpCode == 200) {
        payloadTaf = http.getString();
        http.end();
        break; 
      }
      Serial.printf("[TAF] Attempt %d failed: %d (%s)\n", i + 1, httpCode, http.errorToString(httpCode).c_str());
      http.end();
    }
    delay(1000);
  }
  
  if (httpCode == 200) {
    DynamicJsonDocument doc(16384);
    deserializeJson(doc, payloadTaf);
    
    display.setFullWindow(); display.firstPage();
    do {
      display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
      display.setFont(&FreeSansBold18pt7b); display.setCursor(20, 45); display.print(icao + " TAF");
      if (icao != target) { display.setFont(&FreeSans9pt7b); display.print(" (Ref: " + target + ")"); }
      display.drawLine(20, 60, 770, 60, FG_COLOR);
      
      drawBattery(761, 2);

      if (doc.size() == 0 || doc[0]["fcsts"].isNull()) {
        display.setFont(&FreeSans12pt7b); display.setCursor(20, 120); 
        display.print("TAF UNAVAILABLE, CHECK CLOSEST AIRPORT");
      } else {
        JsonArray fcsts = doc[0]["fcsts"];
        int yOffset = 105;
        for (int i = 0; i < fcsts.size() && i < 4; i++) {
          display.setFont(&FreeSansBold12pt7b);
          time_t tFrom = fcsts[i]["timeFrom"].as<long>();
          struct tm zuluTm, localTm;
          gmtime_r(&tFrom, &zuluTm); localtime_r(&tFrom, &localTm);
          display.setCursor(20, yOffset);
          display.printf("%02d/%02dZ (%02d:00L)", zuluTm.tm_mday, zuluTm.tm_hour, localTm.tm_hour);
          
          display.setFont(&FreeSans12pt7b); display.setCursor(250, yOffset); 
          if (fcsts[i]["wgst"]) display.printf("%03d@%dG%d kt", (int)fcsts[i]["wdir"], (int)fcsts[i]["wspd"], (int)fcsts[i]["wgst"]);
          else display.printf("%03d@%d kt", (int)fcsts[i]["wdir"], (int)fcsts[i]["wspd"]);

          display.setCursor(430, yOffset); display.print(fcsts[i]["visib"].as<String>() + "SM");
          display.setCursor(520, yOffset); 
          display.print(expandWx(fcsts[i]["wxString"] | "") + " " + expandCover(fcsts[i]["clouds"][0]["cover"] | "SKC"));
          yOffset += 45;
        }
      }
    } while (display.nextPage());
  } else {
    drawErrorScreen(icao, "TAF", httpCode);
  }
}

void fetchWinds(String icao) {
  float lat = aptLat[currentIdx];
  float lon = aptLon[currentIdx];

  // Only look up coordinates if a custom slot has 0.0 cached
  if (lat == 0.0 && lon == 0.0) {
    String locUrl = "https://aviationweather.gov/api/data/stationinfo?ids=" + icao + "&format=json";
    WiFiClientSecure sClient;
    sClient.setInsecure();
    HTTPClient httpLoc;
    httpLoc.useHTTP10(true);
    if (httpLoc.begin(sClient, locUrl)) {
      httpLoc.setConnectTimeout(5000);
      httpLoc.setTimeout(10000);
      httpLoc.setReuse(false);
      httpLoc.setUserAgent("MFD-FlightDeck/1.0");
      
      if (httpLoc.GET() == 200) {
        DynamicJsonDocument docLoc(2048);
        deserializeJson(docLoc, httpLoc.getStream());
        if (docLoc.size() > 0) {
          lat = docLoc[0]["lat"] | 37.6934;
          lon = docLoc[0]["lon"] | -121.8204;
          aptLat[currentIdx] = lat;
          aptLon[currentIdx] = lon;
          prefs.begin("mfd", false);
          prefs.putFloat(("lat" + String(currentIdx)).c_str(), lat);
          prefs.putFloat(("lon" + String(currentIdx)).c_str(), lon);
          prefs.end();
        }
      }
      httpLoc.end();
    }

    if (lat == 0.0 && lon == 0.0) {
      lat = 37.6934;
      lon = -121.8204;
    }
  }

  // V32.17: HTTPS /v1/forecast with useHTTP10(true) set BEFORE begin() & 10s read timeout
  int httpCode = -1;
  DynamicJsonDocument doc(4096);
  DeserializationError err = DeserializationError::EmptyInput;

  String windUrl = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) +
                   "&longitude=" + String(lon, 4) +
                   "&current=wind_speed_900hPa,wind_direction_900hPa,temperature_900hPa,"
                   "wind_speed_800hPa,wind_direction_800hPa,temperature_800hPa,"
                   "wind_speed_700hPa,wind_direction_700hPa,temperature_700hPa,"
                   "wind_speed_600hPa,wind_direction_600hPa,temperature_600hPa,"
                   "wind_speed_500hPa,wind_direction_500hPa,temperature_500hPa"
                   "&wind_speed_unit=kn";
  
  for (int i = 0; i < 3; i++) {
    unsigned long t0 = millis();
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    if (http.begin(client, windUrl)) {
      http.setConnectTimeout(6000);
      http.setTimeout(10000);
      http.setReuse(false);
      http.setUserAgent("MFD-FlightDeck/1.0");
      
      httpCode = http.GET();
      Serial.printf("[WINDS] Attempt %d -> HTTP %d (%s) in %lu ms\n",
                    i + 1, httpCode, http.errorToString(httpCode).c_str(), millis() - t0);
      if (httpCode == 200) {
        err = deserializeJson(doc, http.getStream());
        http.end();
        break;
      }
      http.end();
    }
    delay(800);
  }
  
  if (httpCode == 200) {
    JsonObject cur = doc["current"];

    int wdir[5], wspd[5];
    float wtmp[5];
    const char* spdKeys[] = {"wind_speed_900hPa", "wind_speed_800hPa", "wind_speed_700hPa", "wind_speed_600hPa", "wind_speed_500hPa"};
    const char* dirKeys[] = {"wind_direction_900hPa", "wind_direction_800hPa", "wind_direction_700hPa", "wind_direction_600hPa", "wind_direction_500hPa"};
    const char* tmpKeys[] = {"temperature_900hPa", "temperature_800hPa", "temperature_700hPa", "temperature_600hPa", "temperature_500hPa"};

    for (int i = 0; i < 5; i++) {
      wspd[i] = (int)round(cur[spdKeys[i]] | 0.0);
      wdir[i] = (int)round(cur[dirKeys[i]] | 0.0);
      wtmp[i] = cur[tmpKeys[i]] | 0.0;
    }

    display.setFullWindow(); display.firstPage();
    do {
      display.fillScreen(BG_COLOR); display.setTextColor(FG_COLOR);
      display.setFont(&FreeSansBold18pt7b);
      display.setCursor(20, 45);
      display.print(icao + " Winds Aloft");
      display.setFont(&FreeSans12pt7b);
      display.print(" (NOAA GFS Direct)");
      display.drawLine(20, 60, 770, 60, FG_COLOR);
      
      drawBattery(761, 2);
      
      if (err || cur.isNull()) {
        display.setFont(&FreeSans12pt7b);
        display.setCursor(20, 120);
        display.print("NO WINDS DATA AVAILABLE.");
      } else {
        int colX[] = {20, 150, 278, 406, 534, 662};
        const char* headers[] = {"Format", "3,000 ft", "6,000 ft", "10,000 ft", "14,000 ft", "18,000 ft"};
        
        display.setFont(&FreeSansBold12pt7b);
        for (int i = 0; i < 6; i++) {
          display.setCursor(colX[i], 105);
          display.print(headers[i]);
        }
        display.drawLine(20, 120, 770, 120, FG_COLOR);
        
        // Row 1: Decoded (e.g., 270°@14kt)
        display.setFont(&FreeSansBold12pt7b);
        display.setCursor(colX[0], 165);
        display.print("Decoded:");
        for (int i = 0; i < 5; i++) {
          printWindDecoded(colX[i + 1], 165, wdir[i], wspd[i]);
        }

        // Row 2: Classic FAA Encoded (e.g., 2714+11)
        display.setFont(&FreeSansBold12pt7b);
        display.setCursor(colX[0], 215);
        display.print("FAA Fmt:");
        display.setFont(&FreeSans12pt7b);
        for (int i = 0; i < 5; i++) {
          display.setCursor(colX[i + 1], 215);
          display.print(formatFAAWind(wdir[i], wspd[i], wtmp[i]));
        }

        // Footer metadata
        display.drawLine(20, 238, 770, 238, FG_COLOR);
        display.setFont(&FreeSans9pt7b);
        display.setCursor(20, 262);
        char metaBuf[90];
        sprintf(metaBuf, "Model: NOAA HRRR/GFS (%.2f, %.2f) | Pressure Levels: 900-500 hPa", lat, lon);
        display.print(metaBuf);
      }
    } while (display.nextPage());
  } else {
    drawErrorScreen(icao, "Winds Aloft", httpCode);
  }
}

// ==========================================
// STRING EXPANDERS & DRAWING LOGIC 
// ==========================================

void printWindDecoded(int x, int y, int dir, int spd) {
  display.setFont(&FreeSans9pt7b);
  char dirBuf[6];
  sprintf(dirBuf, "%03d", dir);
  display.setCursor(x, y);
  display.print(dirBuf);

  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(dirBuf, x, y, &bx, &by, &bw, &bh);
  
  display.drawCircle(x + bw + 4, y - 10, 2, FG_COLOR);
  
  char spdBuf[12];
  sprintf(spdBuf, "@%dkt", spd);
  display.setCursor(x + bw + 8, y);
  display.print(spdBuf);
}

String formatFAAWind(int dir, int spd, float temp) {
  int dd = ((dir + 5) / 10) % 36;
  if (dd == 0) dd = 36;
  int ss = spd;

  if (ss < 5) {
    dd = 99;
    ss = 0;
  } else if (ss >= 100) {
    dd += 50;
    ss -= 100;
  }

  int tt = (int)round(temp);
  char buf[16];
  sprintf(buf, "%02d%02d%+03d", dd, ss, tt);
  return String(buf);
}

void drawBattery(int x, int y) {
  float pct = 1.0 - ((float)fetchCount / (float)MAX_FETCHES);
  if (pct < 0) pct = 0;
  
  display.fillRect(x, y + 3, 3, 6, FG_COLOR); 
  display.drawRect(x + 3, y, 26, 12, FG_COLOR);
  
  if (pct > 0.66) display.fillRect(x + 5, y + 2, 7, 8, FG_COLOR);  
  if (pct > 0.33) display.fillRect(x + 13, y + 2, 6, 8, FG_COLOR); 
  if (pct > 0.05) display.fillRect(x + 20, y + 2, 7, 8, FG_COLOR); 
}

String formatAlt(int feet) {
  if (feet < 1000) return String(feet);
  if (feet % 1000 == 0) return String(feet / 1000) + "K";
  return String(feet / 1000.0, 1) + "K";
}

String expandCover(String c) {
  if (c=="SKC" || c=="CLR") return "Clear"; 
  if (c=="FEW") return "Few"; 
  if (c=="SCT") return "Sct";
  if (c=="BKN") return "Bkn"; 
  if (c=="OVC") return "Ovc"; 
  return c;
}

String expandWx(String w) {
  w.replace("-", "Lt "); w.replace("+", "Hvy "); w.replace("SH", "Showers ");
  w.replace("RA", "Rain "); w.replace("TS", "TS "); w.trim(); return w;
}

void printWithDegree(int x, int y, const char* label, float val) {
  char buf[20];
  sprintf(buf, "%s: %.1f", label, val);
  display.setCursor(x, y);
  display.print(buf);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(buf, x, y, &bx, &by, &bw, &bh);
  display.drawCircle(x + bw + 6, y - 13, 3, FG_COLOR);
  display.drawCircle(x + bw + 6, y - 13, 2, FG_COLOR);
  display.setCursor(x + bw + 14, y);
  display.print("C");
}

void drawWaterDrop(int x, int y, int r) { 
  display.fillCircle(x, y, r, FG_COLOR);
  display.fillTriangle(x - r, y, x + r, y, x, y - (r * 2), FG_COLOR);
}

void drawEinkSlash(int px, int py, int len, int ang) {
  float rad = ang * M_PI / 180.0;
  float cosA = cos(rad);
  float sinA = sin(rad);

  float steps = len;
  float x_inc = sinA;
  float y_inc = cosA;
  float x = px;
  float y = py;

  for (int i = 0; i <= steps; i++) {
    display.fillCircle((int)x, (int)y, 1, FG_COLOR); 
    x += x_inc;
    y += y_inc;
  }
}

void drawWeatherIcon(int x, int y, String raw, bool isNight) {
  if (raw.indexOf("TS") > 0) { 
    display.fillCircle(x, y, 22, FG_COLOR);
    display.fillCircle(x - 20, y + 10, 16, FG_COLOR);
    display.fillCircle(x + 20, y + 10, 16, FG_COLOR);
    display.fillRect(x - 20, y + 10, 40, 16, FG_COLOR);
    display.fillTriangle(x + 5, y + 15, x - 10, y + 35, x + 5, y + 35, FG_COLOR);
    display.fillTriangle(x - 2, y + 35, x + 12, y + 35, x - 5, y + 55, FG_COLOR);
  } else if (raw.indexOf(" RA") > 0 || raw.indexOf("RA ") > 0) {
    display.fillCircle(x, y, 22, FG_COLOR);
    display.fillCircle(x - 20, y + 10, 16, FG_COLOR);
    display.fillCircle(x + 20, y + 10, 16, FG_COLOR);
    display.fillRect(x - 20, y + 10, 40, 16, FG_COLOR);
    
    int cloudBelly = y + 26;
    int len = 15; 
    int ang = 15; 

    drawEinkSlash(x - 22, cloudBelly + 3, len, ang);
    drawEinkSlash(x - 12, cloudBelly + 6, len, ang);
    drawEinkSlash(x - 2, cloudBelly + 3, len, ang);
    drawEinkSlash(x + 8, cloudBelly + 6, len, ang);
    drawEinkSlash(x + 18, cloudBelly + 3, len, ang);
    drawEinkSlash(x + 28, cloudBelly + 6, len, ang);

  } else if (raw.indexOf("BKN") > 0 || raw.indexOf("OVC") > 0) {
    display.fillCircle(x, y, 22, FG_COLOR);
    display.fillCircle(x - 20, y + 10, 16, FG_COLOR);
    display.fillCircle(x + 20, y + 10, 16, FG_COLOR);
    display.fillRect(x - 20, y + 10, 40, 16, FG_COLOR);
  } else if (raw.indexOf("SCT") > 0 || raw.indexOf("FEW") > 0) {
    if (isNight) {
      int mx = x + 15; int my = y - 15;
      display.fillCircle(mx, my, 16, FG_COLOR);
      display.fillCircle(mx + 6, my - 6, 14, BG_COLOR); 
    } else {
      int sx = x + 15; int sy = y - 15;
      display.drawCircle(sx, sy, 12, FG_COLOR);
      display.drawCircle(sx, sy, 11, FG_COLOR);
      for(int i=0; i<360; i+=45) {
        float rad = i * M_PI / 180.0;
        display.drawLine(sx + 16*cos(rad), sy + 16*sin(rad), sx + 24*cos(rad), sy + 24*sin(rad), FG_COLOR);
      }
    }
    display.fillCircle(x, y, 25, BG_COLOR);
    display.fillCircle(x - 20, y + 10, 19, BG_COLOR);
    display.fillCircle(x + 20, y + 10, 19, BG_COLOR);
    display.fillRect(x - 20, y + 7, 40, 22, BG_COLOR);
    
    display.fillCircle(x, y, 22, FG_COLOR);
    display.fillCircle(x - 20, y + 10, 16, FG_COLOR);
    display.fillCircle(x + 20, y + 10, 16, FG_COLOR);
    display.fillRect(x - 20, y + 10, 40, 16, FG_COLOR);
  } else {
    if (isNight) {
      display.fillCircle(x, y, 28, FG_COLOR);
      display.fillCircle(x + 9, y - 8, 22, BG_COLOR); 
    } else {
      display.drawCircle(x, y, 16, FG_COLOR);
      display.drawCircle(x, y, 15, FG_COLOR);
      display.fillCircle(x - 6, y - 4, 2, FG_COLOR); 
      display.fillCircle(x + 6, y - 4, 2, FG_COLOR); 
      display.drawPixel(x - 6, y + 3, FG_COLOR);     
      display.drawPixel(x - 5, y + 4, FG_COLOR);
      display.drawLine(x - 4, y + 5, x + 4, y + 5, FG_COLOR);
      display.drawPixel(x + 5, y + 4, FG_COLOR);
      display.drawPixel(x + 6, y + 3, FG_COLOR);
      for(int i=0; i<360; i+=45) {
        float rad = i * M_PI / 180.0;
        display.drawLine(x + 22*cos(rad), y + 22*sin(rad), x + 35*cos(rad), y + 35*sin(rad), FG_COLOR);
      }
    }
  }
}

void drawWindsock(int px, int py, int wdir, int wspd) {
  display.fillRect(px - 2, py, 4, 45, FG_COLOR);
  int attachY = py + 2;
  bool pointRight = (wdir >= 180 || wdir == 0); 
  float droopDeg = 85.0; 
  if (wspd >= 3) droopDeg = 60.0;
  if (wspd >= 6) droopDeg = 45.0;
  if (wspd >= 9) droopDeg = 30.0;
  if (wspd >= 12) droopDeg = 15.0;
  if (wspd >= 15) droopDeg = 0.0; 
  float angle = (pointRight ? droopDeg : (180.0 - droopDeg)) * M_PI / 180.0;
  int segments = 5;
  float segLen = 7.0;
  float wStart = 14.0; 
  float wEnd = 5.0;   
  float cosA = cos(angle);
  float floatSin = sin(angle); 
  for (int i = 0; i < segments; i++) {
    float cw = wStart - ((wStart - wEnd) * ((float)i / segments));
    float nw = wStart - ((wStart - wEnd) * ((float)(i + 1) / segments));
    int cx1 = px + (i * segLen) * cosA;
    int cy1 = attachY + (i * segLen) * floatSin;
    int cx2 = px + ((i + 1) * segLen) * cosA;
    int cy2 = attachY + ((i + 1) * floatSin) * segLen;
    int x1 = cx1 + floatSin * (cw / 2); int y1 = cy1 - cosA * (cw / 2);
    int x2 = cx1 - floatSin * (cw / 2); int y2 = cy1 + cosA * (cw / 2);
    int x3 = cx2 - floatSin * (nw / 2); int y3 = cy2 + cosA * (nw / 2);
    int x4 = cx2 + floatSin * (nw / 2); int y4 = cy2 - cosA * (nw / 2);
    if (i % 2 == 0) {
      display.fillTriangle(x1, y1, x2, y2, x3, y3, FG_COLOR);
      display.fillTriangle(x1, y1, x3, y3, x4, y4, FG_COLOR);
    } else {
      display.drawLine(x1, y1, x4, y4, FG_COLOR);
      display.drawLine(x2, y2, x3, y3, FG_COLOR);
    }
  }
}

void drawCompass(int cx, int cy, int r, int wdir, int wspd, int wgst) {
  display.drawCircle(cx, cy, r, FG_COLOR);
  display.drawCircle(cx, cy, r - 2, FG_COLOR);
  display.drawCircle(cx, cy, r - 28, FG_COLOR);
  for (int i = 0; i < 360; i += 45) {
    float rad = (i - 90) * M_PI / 180.0;
    display.drawLine(cx + (r - 2) * cos(rad), cy + (r - 2) * sin(rad), cx + (r - 8) * cos(rad), cy + (r - 8) * sin(rad), FG_COLOR);
  }
  display.setFont(&FreeSans9pt7b);
  display.setCursor(cx - 6, cy - r - 8); display.print("N");
  display.setCursor(cx - 6, cy + r + 20); display.print("S");
  display.setCursor(cx + r + 8, cy + 5); display.print("E");
  display.setCursor(cx - r - 22, cy + 5); display.print("W");
  
  int16_t bx, by; uint16_t bw, bh;
  display.setFont(&FreeSansBold12pt7b);
  if (wspd == 0 && wdir == 0) {
    display.getTextBounds("CALM", 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(cx - (bw / 2), cy + 6);
    display.print("CALM");
  } else if (wdir == 0 && wspd > 0) {
    display.getTextBounds("VRBL", 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(cx - (bw / 2), cy - 2);
    display.print("VRBL");
    String spdStr = String(wspd);
    if (wgst > 0) spdStr += "-" + String(wgst);
    display.setFont(&FreeSans9pt7b);
    display.getTextBounds(spdStr + " kt", 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(cx - (bw / 2), cy + 18);
    display.print(spdStr + " kt");
  } else {
    char dirBuf[4];
    sprintf(dirBuf, "%03d", wdir);
    String dirStr = String(dirBuf);
    
    display.getTextBounds(dirStr, 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(cx - (bw / 2) - 2, cy - 2); 
    display.print(dirStr);
    display.drawCircle(cx + (bw / 2) + 6, cy - 15, 3, FG_COLOR);
    display.drawCircle(cx + (bw / 2) + 6, cy - 15, 2, FG_COLOR);
    String spdStr = String(wspd);
    if (wgst > 0) spdStr += "-" + String(wgst);
    display.setFont(&FreeSans9pt7b);
    display.getTextBounds(spdStr + " kt", 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(cx - (bw / 2), cy + 18); 
    display.print(spdStr + " kt");
    float rad = (wdir - 90) * M_PI / 180.0;
    int x1 = cx + (r - 10) * cos(rad);
    int y1 = cy + (r - 10) * sin(rad);
    int x2 = cx + (r - 26) * cos(rad - 0.15);
    int y2 = cy + (r - 26) * sin(rad - 0.15);
    int x3 = cx + (r - 26) * cos(rad + 0.15);
    int y3 = cy + (r - 26) * sin(rad + 0.15);
    display.fillTriangle(x1, y1, x2, y2, x3, y3, FG_COLOR);
  }
}

void drawTFRIndicator(int cx, int cy) {
  display.drawRect(cx - 45, cy, 90, 30, FG_COLOR);
  display.drawRect(cx - 44, cy + 1, 88, 28, FG_COLOR); 
  display.setFont(&FreeSansBold12pt7b);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds("TFR", 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(cx - (bw / 2), cy + (30/2) + (bh/2) - 2); 
  display.print("TFR");
}

String getBestRunway(String icao, int wdir, int aptIdx) {
  if (wdir == 0) return "Calm / TwrCtl";

  String rwyStr = runways[aptIdx];
  
  if (rwyStr == "") {
    int bestHd = (wdir + 5) / 10;
    if (bestHd == 0 || bestHd == 36) return "36 (Est)";
    String br = String(bestHd);
    if (br.length() == 1) br = "0" + br;
    return br + " (Est)";
  }

  int minDiff = 360;
  String bestRwy = "";
  int start = 0;

  while (start < rwyStr.length()) {
    int comma = rwyStr.indexOf(',', start);
    if (comma == -1) comma = rwyStr.length();
    
    String token = rwyStr.substring(start, comma);
    token.trim(); 

    int hdg = 0;
    String numStr = "";
    for(int i=0; i<token.length(); i++) {
      if(isDigit(token[i]) && numStr.length() < 2) { 
        numStr += token[i];
      } else if (!isDigit(token[i]) && numStr.length() > 0) {
        break; 
      }
    }

    if (numStr.length() > 0) {
      hdg = numStr.toInt() * 10;
      int diff = abs(wdir - hdg);
      if (diff > 180) diff = 360 - diff;
      
      if (diff < minDiff) {
        minDiff = diff;
        bestRwy = token; 
      }
    }
    start = comma + 1;
  }

  if (bestRwy != "") return bestRwy;
  return "N/A";
}
