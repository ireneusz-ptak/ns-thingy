#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "Free_Fonts.h"

//–––––– SD Card Configuration ––––––
#define SD_CLK_PIN    14
#define SD_CMD_PIN    15
#define SD_D0_PIN     16
#define SD_D1_PIN     18
#define SD_D2_PIN     17
#define SD_D3_PIN     21

#define PIN_NEOPIXEL  38

// –––––– Timing Configuration (in milliseconds) ––––––
const unsigned long DATA_FETCH_INTERVAL_MS   = 30 * 1000;
const unsigned long STATUS_FETCH_INTERVAL_MS = 60 * 60 * 1000;
const unsigned long DISPLAY_UPDATE_INTERVAL_MS = 10 * 1000;
const unsigned long STALE_DATA_THRESHOLD_MS = 13 * 60 * 1000;

// –––––– API Configuration ––––––
const int JSON_DOC_SIZE = 8192;

//–––––– TFT_eSPI Objects ––––––
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
TFT_eSprite trd = TFT_eSprite(&tft);

//–––––– Global configuration variables ––––––
String wifiSSID;
String wifiPassword;
String nightscoutURL;
String accessToken;

bool mmol;
bool useLed;

uint8_t ledIntensity = 64;

// BG levels - Default fallback values. These will be updated from Nightscout.
int lowUrgent = 55;
int lowWarning = 70;
int highWarning = 180;
int highUrgent = 240;

//–––––– Global Data & Timing ––––––
unsigned long long lastDataFetchTime = 0;
unsigned long long lastStatusFetchTime = 0;
unsigned long long lastDisplayUpdateTime = 0;
unsigned long long lastUpdate = 0; // board time
unsigned long long lastTimestamp = 0; // NS time

// BG values
float bg;
float delta;
String trend;

void showMessage(String line1, String line2 = "", int delay_ms = 2000) {
    spr.fillSprite(TFT_BLACK);
    spr.setTextDatum(MC_DATUM);
    spr.setTextFont(FONT4);
    spr.drawString(line1, tft.width() / 2, tft.height() / 2 - 10, GFXFF);
    if (line2 != "") {
        spr.drawString(line2, tft.width() / 2, tft.height() / 2 + 20, GFXFF);
    }
    spr.pushSprite(0, 0);
    delay(delay_ms);
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);

  if(!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)){
    showMessage("SD MMC Error", "Pin settings failed!", 5000);
    Serial.println("SD MMC: Pin change failed!");
    while(1);
  }

  spr.createSprite(tft.width(), tft.height());
  spr.setPivot(260, 51);

  if (!SD_MMC.begin()) {
    showMessage("SD Card Error", "Initialization failed.", 5000);
    while(1);
  }

  if (!readConfig()){
    while(1);
  }

  createTrendArrow();
}

void loop() {
  unsigned long currentTime = millis();
  bool dataFetchFailed;
  bool statusFetchFailed;

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if(WiFi.status() == WL_CONNECTED) {
        statusFetchFailed = !fetchStatus();
        dataFetchFailed = !fetchData();
        lastStatusFetchTime = currentTime;
        lastDataFetchTime = currentTime;
    }
    return;
  }

  if (currentTime - lastStatusFetchTime >= STATUS_FETCH_INTERVAL_MS || lastStatusFetchTime == 0) {
    statusFetchFailed = !fetchStatus();
    if (!statusFetchFailed) {
      lastStatusFetchTime = currentTime;
    }
  }

  if (currentTime - lastDataFetchTime >= DATA_FETCH_INTERVAL_MS || lastDataFetchTime == 0) {
    dataFetchFailed = !fetchData();
    if (!dataFetchFailed) {
      lastDataFetchTime = currentTime;
    }
  }

  if (currentTime - lastDisplayUpdateTime >= DISPLAY_UPDATE_INTERVAL_MS && !dataFetchFailed && !statusFetchFailed) {
    updateDisplay();
    updateLED();
    lastDisplayUpdateTime = currentTime;
  }
}

void connectWiFi() {
  setLEDColor(0, 0, ledIntensity);
  showMessage("Connecting to WiFi", wifiSSID, 1000);

  if (wifiSSID != "") {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
      delay(500);
      Serial.print(".");
      retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected.");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      showMessage("WiFi connected!", WiFi.localIP().toString());
    } else {
      Serial.println("\nWiFi connection failed.");
      showMessage("WiFi connection failed", "Check credentials.", 5000);
    }
  } else {
    showMessage("WiFi error", "No SSID defined.", 5000);
  }
  setLEDColor(0, 0, 0);
}

bool readConfig() {
  showMessage("Reading config...", "", 500);
  File configFile = SD_MMC.open("/config.json");

  if (!configFile) {
    Serial.println("Failed to open config.json.");
    showMessage("Config error", "Cannot open /config.json file", 3000);
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    showMessage("Config error", "JSON parse failed", 3000);
    return false;
  }

  wifiSSID      = doc["wifi_ssid"].as<String>();
  wifiPassword  = doc["wifi_password"].as<String>();
  nightscoutURL = doc["nightscout_url"].as<String>();
  accessToken   = doc["access_token"].as<String>();

  mmol          = doc.containsKey("mmol") && doc["mmol"].as<bool>();
  useLed        = doc.containsKey("use_led") && doc["use_led"].as<bool>();

  nightscoutURL.trim();
  while (nightscoutURL.endsWith("/")) {
    nightscoutURL.remove(nightscoutURL.length() - 1);
  }

  if (doc.containsKey("rotate") && doc["rotate"].as<bool>()) {
    tft.setRotation(1);
  }

  Serial.println("Configuration loaded from SD.");
  Serial.println("Nightscout URL: " + nightscoutURL);
  return true;
}

bool fetchStatus() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = nightscoutURL + "/api/v1/status.json";
  if (!accessToken.isEmpty()) {
    url += "?token=" + accessToken;
  }

  Serial.println("Fetching thresholds from: " + url);
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(JSON_DOC_SIZE);
    DeserializationError error = deserializeJson(doc, http.getString());

    if (error) {
      Serial.print("deserializeJson() failed for status: ");
      Serial.println(error.c_str());
      showMessage("NS status fetch error", "JSON parse failed", 2000);
      http.end();
      return false;
    }

    if (doc.containsKey("settings") && doc["settings"].containsKey("thresholds")) {
      JsonObject thresholds = doc["settings"]["thresholds"];

      if (thresholds.containsKey("bgHigh") && thresholds.containsKey("bgTargetTop") &&
          thresholds.containsKey("bgTargetBottom") && thresholds.containsKey("bgLow")) {

        highUrgent = thresholds["bgHigh"].as<int>();
        highWarning = thresholds["bgTargetTop"].as<int>();
        lowWarning = thresholds["bgTargetBottom"].as<int>();
        lowUrgent = thresholds["bgLow"].as<int>();

        String units = doc["settings"]["units"].as<String>();
        if (units.equalsIgnoreCase("mmol") || units.equalsIgnoreCase("mmol/L")) {
            Serial.println("Nightscout units are mmol/L. Converting all thresholds to mg/dL.");
            highUrgent = round(highUrgent * 18.0);
            highWarning = round(highWarning * 18.0);
            lowWarning = round(lowWarning * 18.0);
            lowUrgent = round(lowUrgent * 18.0);
        }

        Serial.println("Thresholds updated from Nightscout:");
        Serial.printf("  Urgent Low:   %d mg/dL\n", lowUrgent);
        Serial.printf("  Warning Low:  %d mg/dL\n", lowWarning);
        Serial.printf("  Warning High: %d mg/dL\n", highWarning);
        Serial.printf("  Urgent High:  %d mg/dL\n", highUrgent);

      } else {
        Serial.println("One or more required threshold keys not found. Using default values.");
      }
    } else {
      Serial.println("Settings/thresholds object not found in API. Using default values.");
    }
  } else {
    Serial.printf("HTTP error on status fetch, code: %d\n", httpCode);
    showMessage("NS status fetch error", "HTTP: " + String(httpCode), 2000);
    http.end();
    return false;
  }
  http.end();
  return true;
}

bool fetchData() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = nightscoutURL + "/api/v2/properties.json";
  if (!accessToken.isEmpty()) {
    url += "?token=" + accessToken;
  }

  Serial.println("Fetching data from: " + url);
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(JSON_DOC_SIZE);
    DeserializationError error = deserializeJson(doc, http.getString());

    if (error) {
      Serial.print("deserializeJson() failed for data: ");
      Serial.println(error.c_str());
      showMessage("NS data fetch error", "JSON parsing failed", 2000);
      http.end();
      return false;
    }

    if (!doc.containsKey("bgnow") || !doc["bgnow"]["sgvs"].is<JsonArray>() || doc["bgnow"]["sgvs"].size() == 0) {
      Serial.println("BG data not found in API response.");
      showMessage("NS data error", "No BG value found", 2000);
      http.end();
      return false;
    }

    unsigned long long newTimestamp = doc["bgnow"]["mills"].as<unsigned long long>();

    if (newTimestamp != lastTimestamp) {
      unsigned long long timestamp = newTimestamp;
      bg = doc["bgnow"]["sgvs"][0]["mgdl"].as<float>();
      delta = doc["delta"]["mgdl"].as<float>();
      trend = doc["bgnow"]["sgvs"][0]["direction"].as<String>();
      if (bg > 30.0) {
        lastTimestamp = timestamp;
        lastUpdate = millis();
        Serial.println("New BG data received.");
        Serial.printf("BG: %.0f mg/dL, Delta: %.1f, Trend: %s\n", bg, delta, trend.c_str());
      } else {
        Serial.println("Invalid BG value (error?).");  
      }
    } else {
      Serial.println("BG data is the same as last fetch.");
    }
  } else {
    Serial.printf("HTTP error on data fetch, code: %d\n", httpCode);
    showMessage("NS data fetch error", "HTTP: " + String(httpCode), 2000);
    http.end();
    return false;
  }
  http.end();
  return true;
}

void updateDisplay() {
  spr.fillSprite(TFT_BLACK);

  if (lastTimestamp == 0) {
      showMessage("Waiting for data...", "", 1000);
      return;
  }

  updateBGValue();
  updateDelta();
  updateTimestamp();

  int16_t arrowAngle = getTrendArrowRotation();
  if (arrowAngle >= 0) {
    trd.pushRotated(&spr, arrowAngle);
    if (isDoubleTrendArrow()) {
      int32_t pivX = spr.getPivotX();
      int32_t pivY = spr.getPivotY();
      spr.setPivot(pivX + 10, pivY);
      trd.pushRotated(&spr, arrowAngle, TFT_BLACK);
      spr.setPivot(pivX, pivY);
    }
  }

  spr.pushSprite(0, 0);
}

String getBGValue(float rawBG) {
  if (mmol) {
    return String(rawBG / 18.0, 1);
  } else {
    return String(rawBG, 0);
  }
}

void updateBGValue() {
  bool isStale = (millis() - lastUpdate) > STALE_DATA_THRESHOLD_MS;
  uint8_t td = spr.getTextDatum();
  spr.setTextDatum(TC_DATUM);
  
  if (isStale) {
    spr.setTextColor(TFT_DARKGREY);
  } else {
    spr.setTextColor(TFT_WHITE);
  }
  
  String bgString = getBGValue(bg);
  spr.drawString(bgString, 120, 15, FONT8);
  
  if (isStale) {
    int16_t textWidth = spr.textWidth(bgString, FONT8);
    int16_t strikeY = 52;
    int16_t strikeX1 = 120 - textWidth / 2;
    int16_t strikeX2 = 120 + textWidth / 2;
    spr.drawLine(strikeX1, strikeY, strikeX2, strikeY, TFT_DARKGREY);
    spr.drawLine(strikeX1, strikeY + 1, strikeX2, strikeY + 1, TFT_DARKGREY);
  }
  
  spr.setTextDatum(td);
  spr.setTextColor(TFT_WHITE);
}

void updateDelta() {
  String prefix = (delta > 0) ? "+" : "";
  uint8_t td = spr.getTextDatum();
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(TFT_LIGHTGREY);
  spr.setFreeFont(FSS24);
  spr.drawString(prefix + getBGValue(delta), 120, 110, GFXFF);
  spr.setTextDatum(td);
  spr.setTextColor(TFT_WHITE);
}

void updateTimestamp() {
  String minutesDisp = "?";

  if (lastUpdate > 0) {
    unsigned long age_ms = millis() - lastUpdate;
    minutesDisp = String(age_ms / 60000);
  }

  spr.setTextDatum(TC_DATUM);

  spr.setFreeFont(FSS18);
  spr.drawString(minutesDisp, 250, 100, GFXFF);
  spr.setFreeFont(FSS12);
  spr.drawString("min. ago", 250, 135, GFXFF);

  spr.setTextColor(TFT_WHITE);
}

void createTrendArrow() {
  trd.createSprite(50, 50);
  trd.fillSprite(TFT_BLACK);
  trd.setPivot(25, 25);
  trd.drawLine(24, 1, 24, 50, TFT_WHITE);
  trd.drawLine(25, 1, 25, 50, TFT_WHITE);
  trd.drawLine(26, 1, 26, 50, TFT_WHITE);
  trd.drawLine(24, 0, 19, 20, TFT_WHITE);
  trd.drawLine(25, 0, 20, 20, TFT_WHITE);
  trd.drawLine(26, 0, 21, 20, TFT_WHITE);
  trd.drawLine(24, 0, 29, 20, TFT_WHITE);
  trd.drawLine(25, 0, 30, 20, TFT_WHITE);
  trd.drawLine(26, 0, 31, 20, TFT_WHITE);
}

int16_t getTrendArrowRotation() {
  if (trend.equalsIgnoreCase("DoubleUp")) return 0;
  if (trend.equalsIgnoreCase("SingleUp")) return 0;
  if (trend.equalsIgnoreCase("FortyFiveUp")) return 45;
  if (trend.equalsIgnoreCase("Flat")) return 90;
  if (trend.equalsIgnoreCase("FortyFiveDown")) return 135;
  if (trend.equalsIgnoreCase("SingleDown")) return 180;
  if (trend.equalsIgnoreCase("DoubleDown")) return 180;
  return -1;
}

bool isDoubleTrendArrow() {
  return trend.startsWith("Double");
}

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  if (useLed) {
    neopixelWrite(PIN_NEOPIXEL, g, r, b);
  }
}

void updateLED() {
  if (lastTimestamp == 0) return;
  // Normal color logic (thresholds are always in mg/dL internally)
  if (bg < lowUrgent || bg > highUrgent) {
    Serial.println("LED: red");
    setLEDColor(ledIntensity, 0, 0);
  } else if (bg < lowWarning || bg > highWarning) {
    Serial.println("LED: yellow");
    setLEDColor(ledIntensity, ledIntensity, 0);
  } else {
    Serial.println("LED: green");
    setLEDColor(0, ledIntensity, 0);
  }
}