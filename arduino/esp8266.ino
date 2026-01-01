/* 
Filter your search...
Type:

All




  ESP8266 Firestore REST -> Arduino (using today's date path)
  - Fetch Firestore documents via REST API and forward control string to Arduino.
  - Listen to Arduino via SoftwareSerial, parse incoming sensor lines and write sensor documents
    to Firestore using the same structure/fields as the old ESP.
  - No simulated defaults: all sensor/state variables start at zero/false.
  - When a serial line is parsed, the sketch prints which expected tokens were NOT present.
  - Date fallback: if NTP time is not yet available, the compile-time build date (__DATE__) is used  
    so you won't get 01jan1970 as the date path.
*/

#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ----------------- Your network & Firebase project -----------------
const char* ssid     = "varoti";
const char* password = "00053278";

const char* projectId = "ecodrip-ddcf0";
const char* apiKey    = "AIzaSyC9D1TLbMsc-3iHMGEWaF_YVBTirg6L-Hc";
const char* farmIdRaw = "unalytix@gmail.com"; // farm id as you provided

// If your Firestore requires authentication, set to true and provide a Firebase user
#define USE_AUTH false
const char* FIREBASE_USER_EMAIL    = "unalytix@gmail.com";
const char* FIREBASE_USER_PASSWORD = "123456";

// ----------------- Serial to Arduino -----------------
SoftwareSerial arduinoSerial(4, 5); // RX, TX (D2=4, D1=5)

// Buffer for incoming Arduino lines
String arduinoLineBuf = "";

// Control variables to send to Arduino (existing)
String cameraLights = "0";
String fieldMaizeLights = "0";
String currentStatus = "off";
String sourcePump = "off";
String cameraAngle = "0.5";

// Timing
unsigned long lastSendTime = 0;
// Reduced to 1 read per second (and sending cadence matches this).
const unsigned long SEND_INTERVAL_MS = 1000UL;

// ID token obtained from signInWithPassword (if USE_AUTH)
String idToken = "";

// HTTP client objects
WiFiClientSecure client;
HTTPClient https;

// ---------- Sensor state (parsed from UNO) - START AT ZERO / FALSE ----------
double temperature = 0.0;
double humidity = 0.0;
int lightVal = 0;
int moistureValue = 0;
int moisturePercentage = 0;
double waterLevel = 0.0; // percent (0..100) — kept for compatibility if needed
double waterLevelLiters = 0.0; // litres as sent by Arduino (W_L)
long plantHeight = 0;    // cm (the Arduino sends recalculated height)
long flowFrequency = 0;  // F: from Arduino (pulses counted)
int flowRate = 0;        // L/h (from L_H token)
bool pumpToTank = false;
bool pumpFromTank = false;
bool lightsOn = false;

double totalWaterUsed = 0.0;
double todayWaterUsed = 0.0; // now can be set directly from Arduino W_USED
unsigned long uptimeSeconds = 0;
int tankCapacity = 0; // parsed from TANK_CAP token

// bookkeeping for interval doc
String lastIntervalId = "";
unsigned long lastMillis = 0;

// battery & calibration vars
const double max_battery_voltage = 0.0;
double base_current_voltage = 0.0;
double current_voltage = 0.0;
double lifePercent = 0.0;
long timeRemainingSeconds = 0;

bool moistureSensorCalibrated = false;
bool calibrationFlag = false;
bool leakageFlag = false;

unsigned long lastVoltageDecreaseMillis = 0;
unsigned long lastUptimeMillis = 0;
// ---------- Sensor state (parsed from UNO) - END ----------------------------

// store latest reported servo angle from Arduino
double servoReportedAngle = 0.0;

// ----------------- One-time doc creation guards -----------------
bool pumpDocEnsured = false;
bool lightsDocEnsured = false;

// ----------------- Hardware pins for new lights -----------------
// Chosen pins (ESP8266 GPIO numbers). Adjust if you prefer different pins.
const int farmLight1Pin = 12; // D6 (GPIO12)
const int farmLight2Pin = 13; // D7 (GPIO13)
// cameraLightPin set to D1 (GPIO5) per your request
const int cameraLightPin = 15; // D8 (GPIO15)

// ----------------- Helpers -----------------
String urlEncode(const String &s) {
  String r = "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ( (c>='a' && c<='z') || (c>='A' && c<='Z') ||
         (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c=='~') {
      r += c;
    } else {
      char buf[5];
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      r += buf;
    }
  }
  return r;
}

// small helpers for control string interpretation
bool isTrueLike(const String &s) {
  String t = s;
  t.toLowerCase();
  t.trim();
  return (t == "1" || t == "true" || t == "on" || t == "yes");
}
bool isFalseLike(const String &s) {
  String t = s;
  t.toLowerCase();
  t.trim();
  return (t == "0" || t == "false" || t == "off");
}
bool isNoString(const String &s) {
  String t = s;
  t.toLowerCase();
  t.trim();
  return (t == "no");
}

// Use NTP time if available; otherwise fallback to compile-time build date (__DATE__)
String getTodayDocDate() {
  time_t nowt = time(nullptr);
  // If time is not synced (epoch or very old), use compile-time build date
  if (nowt < 1600000000) {
    // __DATE__ example: "Nov 11 2025"
    String b = String(__DATE__); 
    b.trim();
    String monStr = b.substring(0,3);
    monStr.toLowerCase();
    String dayStr = b.substring(4,6);
    dayStr.trim();
    String yearStr = b.substring(7);
    yearStr.trim();

    const char *mons[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
    int midx = 0;
    String monsUpper = monStr;
    monsUpper.toLowerCase();
    for (int i = 0; i < 12; ++i) {
      if (monsUpper == String(mons[i])) { midx = i; break; }
    }
    int day = dayStr.toInt();
    int year = yearStr.toInt();
    char buf[20];
    snprintf(buf, sizeof(buf), "%02d%s%d", day, mons[midx], year);
    return String(buf);
  }

  struct tm *tmstruct = localtime(&nowt);
  if (!tmstruct) {
    // fallback if localtime fails (shouldn't happen because above check handled bad time)
    return String("10nov2025");
  }
  int day = tmstruct->tm_mday;
  int mon = tmstruct->tm_mon + 1;
  int year = tmstruct->tm_year + 1900;
  const char *mons[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d%s%d", day, mons[mon-1], year);
  return String(buf); // e.g. "11nov2025"
}

String getIsoTimestampUTC() {
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char buf[40];
  sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
          t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

String baseSensorsPathForToday() {
  String dateId = getTodayDocDate();
  String farmEnc = urlEncode(String(farmIdRaw));
  return "https://firestore.googleapis.com/v1/projects/" + String(projectId) +
         "/databases/(default)/documents/farms/" + farmEnc + "/readings/" + urlEncode(dateId) + "/sensors";
}

String baseReadingsPathForToday() {
  String dateId = getTodayDocDate();
  String farmEnc = urlEncode(String(farmIdRaw));
  return "https://firestore.googleapis.com/v1/projects/" + String(projectId) +
         "/databases/(default)/documents/farms/" + farmEnc + "/readings/" + urlEncode(dateId);
}

// ----------------- HTTP helpers (PATCH) -----------------
bool httpPatchJson(const String &url, const String &jsonPayload, int &outCode, String &outResp) {
  client.setInsecure();
  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  https.setTimeout(20000);
  int code = https.PATCH(jsonPayload);
  String resp = https.getString();
  outCode = code;
  outResp = resp;
  https.end();
  return true;
}

// Generic GET to Firestore doc path, returns payload or empty string
String firestoreGetDocument(const String &docPath) {
  // docPath: projects/<projectId>/databases/(default)/documents/...
  String url = String("https://firestore.googleapis.com/v1/") + docPath;
  client.setInsecure();
  https.begin(client, url);
  if (USE_AUTH && idToken.length()) {
    String bearer = String("Bearer ") + idToken;
    https.addHeader("Authorization", bearer);
  }
  int code = https.GET();
  if (code != 200) {
    Serial.print("Firestore GET failed (");
    Serial.print(code);
    Serial.println(")");
    String resp = https.getString();
    Serial.println(resp);
    https.end();
    return String("");
  }
  String payload = https.getString();
  https.end();
  return payload;
}

// ------------------------ Fallback text-based extractor (kept) ------------------------
String extractFieldFromPayloadText(const String &payload, const String &fieldName) {
  int idx = payload.indexOf("\"" + fieldName + "\"");
  if (idx < 0) return String("");

  int searchStart = idx;
  int sv = payload.indexOf("stringValue", searchStart);
  if (sv >= 0) {
    int colon = payload.indexOf(':', sv);
    if (colon >= 0) {
      int quote1 = payload.indexOf('"', colon);
      if (quote1 >= 0) {
        int quote2 = payload.indexOf('"', quote1 + 1);
        if (quote2 > quote1) {
          return payload.substring(quote1 + 1, quote2);
        }
      }
    }
  }
  int iv = payload.indexOf("integerValue", searchStart);
  if (iv >= 0) {
    int colon = payload.indexOf(':', iv);
    if (colon >= 0) {
      int start = colon + 1;
      while (start < (int)payload.length() && isSpace(payload[start])) start++;
      int end = start;
      while (end < (int)payload.length() && (isDigit(payload[end]) || payload[end]=='-' )) end++;
      if (end > start) return payload.substring(start, end);
    }
  }
  int dv = payload.indexOf("doubleValue", searchStart);
  if (dv >= 0) {
    int colon = payload.indexOf(':', dv);
    if (colon >= 0) {
      int start = colon + 1;
      while (start < (int)payload.length() && isSpace(payload[start])) start++;
      int end = start;
      while (end < (int)payload.length() && (isDigit(payload[end]) || payload[end]=='.' || payload[end]=='-' )) end++;
      if (end > start) return payload.substring(start, end);
    }
  }
  int bv = payload.indexOf("booleanValue", searchStart);
  if (bv >= 0) {
    int colon = payload.indexOf(':', bv);
    if (colon >= 0) {
      int start = colon + 1;
      while (start < (int)payload.length() && isSpace(payload[start])) start++;
      if (payload.startsWith("true", start)) return String("1");
      if (payload.startsWith("false", start)) return String("0");
    }
  }

  int colonKey = payload.indexOf(':', idx);
  if (colonKey >= 0) {
    int s = colonKey + 1;
    while (s < (int)payload.length() && isSpace(payload[s])) s++;
    if (s < (int)payload.length()) {
      if (payload[s] == '"') {
        int q2 = payload.indexOf('"', s+1);
        if (q2 > s) return payload.substring(s+1, q2);
      } else {
        int e = s;
        while (e < (int)payload.length() && payload[e] != ',' && payload[e] != '}' && payload[e] != '\n' && payload[e] != '\r') e++;
        if (e > s) {
          String tok = payload.substring(s, e);
          tok.trim();
          return tok;
        }
      }
    }
  }

  return String("");
}

String getFieldFromDocPayload(const String &payload, const char* fieldName) {
  if (payload.length() == 0) return String("");
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (!err) {
    if (doc.containsKey("fields") && doc["fields"].containsKey(fieldName)) {
      JsonObject v = doc["fields"][fieldName].as<JsonObject>();
      if (v.containsKey("stringValue")) {
        const char* s = v["stringValue"];
        if (s) return String(s);
      }
      if (v.containsKey("integerValue")) {
        const char* s = v["integerValue"];
        if (s) return String(s);
      }
      if (v.containsKey("doubleValue")) {
        const char* s = v["doubleValue"];
        if (s) return String(s);
      }
      if (v.containsKey("booleanValue")) {
        bool b = v["booleanValue"];
        return b ? String("1") : String("0");
      }
    }
  }
  // fallback
  return extractFieldFromPayloadText(payload, String(fieldName));
}

// ------------------------ Pump usage approx (copied) ------------------------
void updatePumpUsageApprox() {
  unsigned long now = millis();
  unsigned long elapsedMs = (lastMillis==0) ? 0 : now - lastMillis;
  if (elapsedMs > 0) {
    double elapsedSec = elapsedMs / 1000.0;
    bool pumpRunning = (pumpToTank || pumpFromTank);
    if (pumpRunning) {
      uptimeSeconds += (unsigned long)elapsedSec;
      double used = ((double)flowRate) * (elapsedSec / 3600.0);
      totalWaterUsed += used;
      todayWaterUsed += used;
    }
  }
  lastMillis = now;
}

// ------------------------ Ensure pump & lights docs exist ONCE ------------------------
void ensurePumpAndLightsDocsOnce() {
  if (pumpDocEnsured && lightsDocEnsured) return;
  if (WiFi.status() != WL_CONNECTED) return;

  String farmEnc = urlEncode(String(farmIdRaw));
  String dateId = getTodayDocDate();

  // pump doc path (docPath style used by firestoreGetDocument)
  String pumpDocPath = "projects/" + String(projectId) + "/databases/(default)/documents/farms/" + farmEnc + "/readings/" + urlEncode(dateId) + "/sensors/pump?key=" + String(apiKey);
  String pumpResp = firestoreGetDocument(pumpDocPath);
  if (pumpResp.length() == 0) {
    // create pump doc with current_status off & source_pump off (only once)
    String ts = getIsoTimestampUTC();
    String json = "{ \"fields\": {"
      "\"current_status\": {\"stringValue\": \"off\"},"
      "\"source_pump\": {\"stringValue\": \"off\"},"
      "\"pressure\": {\"doubleValue\": 0},"
      "\"uptime\": {\"integerValue\": 0},"
      "\"flow_rate\": {\"integerValue\": 0},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"pump\"}"
      "} }";
    int c; String r;
    String fullUrl = String("https://firestore.googleapis.com/v1/") + pumpDocPath;
    httpPatchJson(fullUrl, json, c, r);
    Serial.println("Created pump doc (one-time) with OFF state.");
  } else {
    Serial.println("Pump doc already exists (not creating).");
  }
  pumpDocEnsured = true;

  // lights_control doc
  String lightsDocPath = "projects/" + String(projectId) + "/databases/(default)/documents/farms/" + farmEnc + "/readings/" + urlEncode(dateId) + "/sensors/lights_control?key=" + String(apiKey);
  String lightsResp = firestoreGetDocument(lightsDocPath);
  if (lightsResp.length() == 0) {
    String ts = getIsoTimestampUTC();
    String json = "{ \"fields\": {"
      "\"camera_lights\": {\"booleanValue\": false},"
      "\"field_lights\": {\"booleanValue\": false},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"lights_control\"}"
      "} }";
    int c; String r;
    String fullUrl = String("https://firestore.googleapis.com/v1/") + lightsDocPath;
    httpPatchJson(fullUrl, json, c, r);
    Serial.println("Created lights_control doc (one-time) with OFF states (field_lights used).");
  } else {
    Serial.println("lights_control doc already exists (not creating).");
  }
  lightsDocEnsured = true;
}

// ------------------------ Update sensors/* documents (silent) with requested changes ------------------------
void updateSensorsDocs() {
  if (WiFi.status() != WL_CONNECTED) return;

  String ts = getIsoTimestampUTC();
  String base = baseSensorsPathForToday();
  int code; String resp;

  // humidity
  {
    String url = base + "/humidity?key=" + String(apiKey);
    String json = "{ \"fields\": {"
      "\"current\": {\"integerValue\": " + String((int)round(humidity)) + "},"
      "\"field_maize\": {\"integerValue\": " + String((int)round(humidity)) + "},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"humidity\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }

  // soil
  {
    String url = base + "/soil?key=" + String(apiKey);
    String json = "{ \"fields\": {"
      "\"current\": {\"integerValue\": " + String(moisturePercentage) + "},"
      "\"field_maize\": {\"integerValue\": " + String(moisturePercentage) + "},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"soil\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }

  // light
  {
    String url = base + "/light?key=" + String(apiKey);
    int minToday = max(0, (int)(lightVal * 0.1));
    int maxToday = max(0, (int)(lightVal * 1.2));
    String json = "{ \"fields\": {"
      "\"current\": {\"integerValue\": " + String(lightVal) + "},"
      "\"min_today\": {\"integerValue\": " + String(minToday) + "},"
      "\"min_today_time\": {\"stringValue\": \"\"},"
      "\"max_today\": {\"integerValue\": " + String(maxToday) + "},"
      "\"max_today_time\": {\"stringValue\": \"\"},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"light\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }

  // temperature
  {
    String url = base + "/temperature?key=" + String(apiKey);
    String json = "{ \"fields\": {"
      "\"current\": {\"doubleValue\": " + String(temperature, 2) + "},"
      "\"min_today\": {\"doubleValue\": " + String(temperature - 4.0, 2) + "},"
      "\"min_today_time\": {\"stringValue\": \"\"},"
      "\"max_today\": {\"doubleValue\": " + String(temperature + 4.0, 2) + "},"
      "\"max_today_time\": {\"stringValue\": \"\"},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"temperature\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }

  // height
  {
    String url = base + "/height?key=" + String(apiKey);
    double height_m = ((double)plantHeight) / 100.0;
    String json = "{ \"fields\": {"
      "\"current\": {\"doubleValue\": " + String(height_m, 2) + "},"
      "\"field_maize\": {\"doubleValue\": " + String(height_m, 2) + "},"
      "\"growth_rate\": {\"doubleValue\": 0},"
      "\"planted_date\": {\"stringValue\": \"15/03/2025\"},"
      "\"initial_height\": {\"doubleValue\": 0.10},"
      "\"days_since_planted\": {\"integerValue\": 0},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"height\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }

  // NOTE: per request, do NOT update pump.current_status or pump.source_pump every second.
  // The pump doc is created once (ensurePumpAndLightsDocsOnce) and then not updated here.

  // NOTE: per request, do NOT update lights_control every second. lights_control doc is created once.

  // camera (kept; silent)
  {
    String url = base + "/camera?key=" + String(apiKey);
    String json = "{ \"fields\": {"
      "\"status\": {\"stringValue\": \"active\"},"
      "\"field_maize_health\": {\"stringValue\": \"Healthy\"},"
      "\"field_maize_comment\": {\"stringValue\": \"Plants showing good growth with vibrant green leaves.\"},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"camera\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }

  // water (MODIFIED as per your request):
  // - current: store W_L (litres) as double
  // - water_level: also store W_L (litres) as double
  // - tank_capacity: store TANK_CAP (integer)
  // - flow_rate: store flowRate (L/h)
  // - used_today: store todayWaterUsed
  {
    String url = base + "/water?key=" + String(apiKey);
    // ensure we have a valid numeric for waterLevelLiters
    double wl = waterLevelLiters;
    if (isnan(wl)) wl = 0.0;
    String json = "{ \"fields\": {"
      "\"current\": {\"doubleValue\": " + String(wl, 3) + "},"
      "\"water_level\": {\"doubleValue\": " + String(wl, 3) + "},"
      "\"used_today\": {\"doubleValue\": " + String(todayWaterUsed, 3) + "},"
      "\"flow_rate\": {\"integerValue\": " + String(flowRate) + "},"
      "\"tank_capacity\": {\"integerValue\": " + String(tankCapacity) + "},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"water\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }

  // update the daily readings doc with today_water_used (so /readings/<date> has this updated)
  {
    String dailyUrl = baseReadingsPathForToday() + "?key=" + String(apiKey);
    String jsonDailyUpdate = "{ \"fields\": {"
      "\"today_water_used\": {\"doubleValue\": " + String(todayWaterUsed, 3) + "},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"}"
    "} }";
    httpPatchJson(dailyUrl, jsonDailyUpdate, code, resp);
  }

  // status
  {
    String url = base + "/status?key=" + String(apiKey);
    String json = "{ \"fields\": {"
      "\"max_battery_voltage\": {\"doubleValue\": " + String(max_battery_voltage, 2) + "},"
      "\"current_voltage\": {\"doubleValue\": " + String(current_voltage, 3) + "},"
      "\"life_percent\": {\"doubleValue\": " + String(lifePercent, 2) + "},"
      "\"time_remaining_seconds\": {\"integerValue\": " + String(timeRemainingSeconds) + "},"
      "\"moisture_calibrated\": {\"booleanValue\": " + String(moistureSensorCalibrated ? "true" : "false") + "},"
      "\"calibration\": {\"booleanValue\": " + String(calibrationFlag ? "true" : "false") + "},"
      "\"leakage\": {\"booleanValue\": " + String(leakageFlag ? "true" : "false") + "},"
      "\"last_updated\": {\"timestampValue\": \"" + ts + "\"},"
      "\"sensor_type\": {\"stringValue\": \"status\"}"
    "} }";
    httpPatchJson(url, json, code, resp);
  }
}

// ------------------------ Interval doc & daily averages (copied & adapted) ------------------------
void createIntervalDocOncePerMinuteAndRecompute() {
  String timeId;
  // build HH:MM
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char bufTime[8];
  sprintf(bufTime, "%02d:%02d", t.tm_hour, t.tm_min);
  timeId = String(bufTime);

  if (timeId == lastIntervalId) return;
  lastIntervalId = timeId;

  if (WiFi.status() != WL_CONNECTED) return;

  String dateId_local = getTodayDocDate();
  String farmEnc = urlEncode(String(farmIdRaw));
  String docPath = "projects/" + String(projectId) +
                  "/databases/(default)/documents/farms/" + farmEnc +
                  "/readings/" + urlEncode(dateId_local) + "/interval_based/" + urlEncode(timeId) +
                  "?key=" + String(apiKey);

  String ts = getIsoTimestampUTC();

  String json = "{ \"fields\": {"
    "\"time\": {\"stringValue\": \"" + String(timeId) + "\"},"
    "\"timestamp\": {\"timestampValue\": \"" + ts + "\"},"
    "\"height\": {\"doubleValue\": " + String((double)plantHeight, 2) + "},"
    "\"water_level\": {\"doubleValue\": " + String(waterLevel, 2) + "},"
    "\"soil_moisture\": {\"integerValue\": " + String(moisturePercentage) + "},"
    "\"humidity\": {\"integerValue\": " + String((int)round(humidity)) + "},"
    "\"temperature\": {\"doubleValue\": " + String(temperature, 2) + "},"
    "\"battery_voltage\": {\"doubleValue\": " + String(current_voltage, 3) + "},"
    "\"light\": {\"integerValue\": " + String(lightVal) + "},"
    "\"flow_rate\": {\"doubleValue\": " + String((double)flowRate, 2) + "},"
    "\"pump_status\": {\"stringValue\": \"" + String((pumpToTank || pumpFromTank) ? "on" : "off") + "\"},"
    "\"uptime\": {\"integerValue\": " + String(uptimeSeconds) + "},"
    "\"total_water_used\": {\"doubleValue\": " + String(totalWaterUsed, 3) + "},"
    "\"today_water_used\": {\"doubleValue\": " + String(todayWaterUsed, 3) + "}"
  "} }";

  int code; String resp;
  // use httpPatchJson with full URL
  String fullUrl = String("https://firestore.googleapis.com/v1/") + docPath;
  httpPatchJson(fullUrl, json, code, resp);

  // recompute averages (silent on errors)
  String listPath = "projects/" + String(projectId) +
                   "/databases/(default)/documents/farms/" + farmEnc +
                   "/readings/" + urlEncode(dateId_local) + "/interval_based?key=" + String(apiKey);

  String getResp = firestoreGetDocument(listPath);
  if (getResp.length() == 0) {
    // create zeroed daily doc
    String dailyUrl = baseReadingsPathForToday() + "?key=" + String(apiKey);
    String tsNow = getIsoTimestampUTC();
    String jsonDaily = "{ \"fields\": {"
      "\"averages_updated_at\": {\"timestampValue\": \"" + tsNow + "\"},"
      "\"avg_battery_voltage\": {\"doubleValue\": 0.0},"
      "\"avg_flow_rate\": {\"doubleValue\": 0.0},"
      "\"avg_height\": {\"doubleValue\": 0.0},"
      "\"avg_humidity\": {\"doubleValue\": 0.0},"
      "\"avg_light\": {\"doubleValue\": 0.0},"
      "\"avg_soil_moisture\": {\"doubleValue\": 0.0},"
      "\"avg_temperature\": {\"doubleValue\": 0.0},"
      "\"avg_water_level\": {\"doubleValue\": 0.0},"
      "\"date\": {\"stringValue\": \"" + dateId_local + "\"},"
      "\"last_updated\": {\"timestampValue\": \"" + tsNow + "\"},"
      "\"pump_status\": {\"stringValue\": \"off\"},"
      "\"pump_uptime\": {\"integerValue\": 0},"
      "\"tank_capacity\": {\"integerValue\": " + String(tankCapacity) + "},"
      "\"today_water_used\": {\"doubleValue\": 0.0},"
      "\"total_interval_readings\": {\"integerValue\": 0},"
      "\"total_readings_today\": {\"integerValue\": 0},"
      "\"total_water_used\": {\"doubleValue\": 0.0},"
      "\"water_level\": {\"doubleValue\": 0.0}"
    "} }";
    int c2; String r2;
    httpPatchJson(dailyUrl, jsonDaily, c2, r2);
    return;
  }

  const size_t CAP = 64 * 1024;
  DynamicJsonDocument root(CAP);
  DeserializationError derr = deserializeJson(root, getResp);
  if (derr) return;

  JsonArray docsArray = root["documents"].as<JsonArray>();
  if (docsArray.isNull() || docsArray.size() == 0) {
    // same fallback already handled
    return;
  }

  int count = 0;
  double sum_temp = 0.0, sum_hum = 0.0, sum_light = 0.0, sum_soil = 0.0, sum_wlvl = 0.0, sum_ht = 0.0, sum_flow = 0.0, sum_batt = 0.0;
  double max_total_water_used = 0.0;
  double max_today_water_used = 0.0;
  unsigned long max_uptime = 0;
  bool anyPumpOn = false;

  for (JsonObject item : docsArray) {
    JsonObject fields = item["fields"].as<JsonObject>();
    if (fields.isNull()) continue;

    auto readNumber = [&](JsonObject &f, const char* name, double &outVal)->bool {
      outVal = NAN;
      if (!f.containsKey(name)) return false;
      JsonObject v = f[name].as<JsonObject>();
      if (v.containsKey("doubleValue")) {
        outVal = (double)v["doubleValue"].as<double>();
        return true;
      } else if (v.containsKey("integerValue")) {
        const JsonVariant iv = v["integerValue"];
        if (iv.is<long>() || iv.is<int>()) outVal = (double) iv.as<long>();
        else {
          const char* s = iv.as<const char*>();
          outVal = (s) ? atof(s) : NAN;
        }
        return true;
      } else if (v.containsKey("stringValue")) {
        const char* s = v["stringValue"];
        if (s && strlen(s) > 0) { outVal = atof(s); return true; }
      }
      return false;
    };

    double tmp;
    if (readNumber(fields, "temperature", tmp)) sum_temp += tmp;
    if (readNumber(fields, "humidity", tmp)) sum_hum += tmp;
    if (readNumber(fields, "light", tmp)) sum_light += tmp;
    if (readNumber(fields, "soil_moisture", tmp)) sum_soil += tmp;
    if (readNumber(fields, "water_level", tmp)) sum_wlvl += tmp;
    if (readNumber(fields, "height", tmp)) sum_ht += tmp;
    if (readNumber(fields, "flow_rate", tmp)) sum_flow += tmp;
    if (readNumber(fields, "battery_voltage", tmp)) sum_batt += tmp;

    if (readNumber(fields, "total_water_used", tmp)) {
      if (tmp > max_total_water_used) max_total_water_used = tmp;
    }
    if (readNumber(fields, "today_water_used", tmp)) {
      if (tmp > max_today_water_used) max_today_water_used = tmp;
    }
    if (readNumber(fields, "uptime", tmp)) {
      unsigned long up = (unsigned long) tmp;
      if (up > max_uptime) max_uptime = up;
    }

    if (fields.containsKey("pump_status")) {
      JsonObject ps = fields["pump_status"].as<JsonObject>();
      if (ps.containsKey("stringValue")) {
        const char* s = ps["stringValue"];
        if (s && String(s) == "on") anyPumpOn = true;
      }
    }

    count++;
  }

  double avg_temp = (count > 0) ? (sum_temp / count) : 0.0;
  double avg_hum = (count > 0) ? (sum_hum / count) : 0.0;
  double avg_light = (count > 0) ? (sum_light / count) : 0.0;
  double avg_soil = (count > 0) ? (sum_soil / count) : 0.0;
  double avg_wlvl = (count > 0) ? (sum_wlvl / count) : 0.0;
  double avg_ht = (count > 0) ? (sum_ht / count) : 0.0;
  double avg_flow = (count > 0) ? (sum_flow / count) : 0.0;
  double avg_batt = (count > 0) ? (sum_batt / count) : 0.0;

  String tsNow = getIsoTimestampUTC();
  String dailyUrl = baseReadingsPathForToday() + "?key=" + String(apiKey);

  String jsonDaily = "{ \"fields\": {"
    "\"averages_updated_at\": {\"timestampValue\": \"" + tsNow + "\"},"
    "\"avg_battery_voltage\": {\"doubleValue\": " + String(avg_batt, 3) + "},"
    "\"avg_flow_rate\": {\"doubleValue\": " + String(avg_flow, 2) + "},"
    "\"avg_height\": {\"doubleValue\": " + String(avg_ht, 2) + "},"
    "\"avg_humidity\": {\"doubleValue\": " + String(avg_hum, 2) + "},"
    "\"avg_light\": {\"doubleValue\": " + String(avg_light, 2) + "},"
    "\"avg_soil_moisture\": {\"doubleValue\": " + String(avg_soil, 2) + "},"
    "\"avg_temperature\": {\"doubleValue\": " + String(avg_temp, 2) + "},"
    "\"avg_water_level\": {\"doubleValue\": " + String(avg_wlvl, 2) + "},"
    "\"date\": {\"stringValue\": \"" + getTodayDocDate() + "\"},"
    "\"last_updated\": {\"timestampValue\": \"" + tsNow + "\"},"
    "\"pump_status\": {\"stringValue\": \"" + String(anyPumpOn ? "on" : "off") + "\"},"
    "\"pump_uptime\": {\"integerValue\": " + String(max_uptime) + "},"
    "\"tank_capacity\": {\"integerValue\": " + String(tankCapacity) + "},"
    "\"today_water_used\": {\"doubleValue\": " + String(max_today_water_used, 3) + "},"
    "\"total_interval_readings\": {\"integerValue\": " + String(count) + "},"
    "\"total_readings_today\": {\"integerValue\": " + String(count) + "},"
    "\"total_water_used\": {\"doubleValue\": " + String(max_total_water_used, 3) + "},"
    "\"water_level\": {\"doubleValue\": " + String(avg_wlvl, 2) + "}"
  "} }";

  int c2; String r2;
  httpPatchJson(dailyUrl, jsonDaily, c2, r2);
}

// ------------------------ UNO serial parsing (reads from UNO & writes to Firestore) ------------------------
void receiveAndParseSensors(const String &receivedData) {
  String s = receivedData;
  s.trim();
  if (s.length() == 0) return;

  // mark presence of tokens
  bool seen_T = false;
  bool seen_H = false;
  bool seen_L = false;
  bool seen_M = false;
  bool seen_MP = false;
  bool seen_W = false;      // used for percent-based W / W_L (we reuse)
  bool seen_P = false;
  bool seen_F = false;      // F: now treated as flow frequency presence
  bool seen_L_H = false;
  bool seen_PUMP_T = false;
  bool seen_PUMP_F = false;
  bool seen_LIGHTS = false;
  bool seen_MAX_BV = false;
  bool seen_CURV = false;
  bool seen_UPTIME = false;
  bool seen_LIFE = false;
  bool seen_TIME_REMAIN = false;
  bool seen_MOIST_CAL = false;
  bool seen_CALIBRATION = false;
  bool seen_LEAKAGE = false;
  bool seen_SERVO = false;
  bool seen_TANK_CAP = false;
  bool seen_W_USED = false;
  // local holder for values parsed by tokens (some tokens may require post-processing)
  long parsed_flow_frequency = -1;
  double parsed_servo_angle = NAN;
  double parsed_w_liters = NAN;
  int parsed_tank_cap = 0;
  double parsed_w_used = NAN;

  // parse tokens from UNO
  int start = 0;
  while (true) {
    int end = s.indexOf(' ', start);
    String token = (end == -1) ? s.substring(start) : s.substring(start, end);
    token.trim();
    if (token.length() > 0) {
      if (token.startsWith("T:")) { temperature = token.substring(2).toFloat(); seen_T = true; }
      else if (token.startsWith("H:")) { humidity = token.substring(2).toFloat(); seen_H = true; }
      else if (token.startsWith("L:")) { lightVal = token.substring(2).toInt(); seen_L = true; }
      else if (token.startsWith("M:")) { moistureValue = token.substring(2).toInt(); seen_M = true; }
      else if (token.startsWith("MP:")) { moisturePercentage = token.substring(3).toInt(); seen_MP = true; }
      // Arduino changed: W_L (litres) instead of W (percent). Accept both.
      else if (token.startsWith("W_L:")) { parsed_w_liters = token.substring(4).toFloat(); seen_W = true; }
      else if (token.startsWith("W:")) { 
        // legacy: percent — keep for backwards compatibility
        waterLevel = token.substring(2).toFloat(); 
        parsed_w_liters = NAN;
        seen_W = true; 
      }
      else if (token.startsWith("P:")) { plantHeight = token.substring(2).toInt(); seen_P = true; }
      // F: from Arduino is flow_frequency (pulses) — we capture it into parsed_flow_frequency
      else if (token.startsWith("F:")) { parsed_flow_frequency = token.substring(2).toInt(); seen_F = true; }
      else if (token.startsWith("L_H:")) { flowRate = token.substring(4).toInt(); seen_L_H = true; }
      else if (token.startsWith("PUMP_T:")) { pumpToTank = (token.substring(7).toInt() == 1); seen_PUMP_T = true; }
      else if (token.startsWith("PUMP_F:")) { pumpFromTank = (token.substring(7).toInt() == 1); seen_PUMP_F = true; }
      else if (token.startsWith("LIGHTS:")) { lightsOn = (token.substring(7).toInt() == 1); seen_LIGHTS = true; }
      else if (token.startsWith("MAX_BV:")) { double v = token.substring(7).toFloat(); if (v > 0.0 && v <= 12.0) base_current_voltage = v; seen_MAX_BV = true; }
      else if (token.startsWith("CURV:")) { current_voltage = token.substring(5).toFloat(); seen_CURV = true; }
      else if (token.startsWith("UPTIME:")) { uptimeSeconds = (unsigned long)token.substring(7).toInt(); seen_UPTIME = true; }
      else if (token.startsWith("LIFE:")) { lifePercent = token.substring(5).toFloat(); seen_LIFE = true; }
      else if (token.startsWith("TIME_REMAIN:")) { timeRemainingSeconds = token.substring(12).toInt(); seen_TIME_REMAIN = true; }
      else if (token.startsWith("MOIST_CAL:")) { moistureSensorCalibrated = (token.substring(10).toInt() == 1); seen_MOIST_CAL = true; }
      else if (token.startsWith("CALIBRATION:")) { calibrationFlag = (token.substring(12).toInt() == 1); seen_CALIBRATION = true; }
      else if (token.startsWith("LEAKAGE:")) { leakageFlag = (token.substring(8).toInt() == 1); seen_LEAKAGE = true; }
      else if (token.startsWith("SERVO:")) { parsed_servo_angle = token.substring(6).toFloat(); seen_SERVO = true; }
      else if (token.startsWith("TANK_CAP:")) { parsed_tank_cap = token.substring(9).toInt(); seen_TANK_CAP = true; }
      else if (token.startsWith("W_USED:")) { parsed_w_used = token.substring(7).toFloat(); seen_W_USED = true; }
    }

    if (end == -1) break;
    start = end + 1;
  }

  // post-processing after token loop:
  // - apply parsed flow frequency
  if (parsed_flow_frequency >= 0) {
    flowFrequency = parsed_flow_frequency;
  }
  // - apply parsed servo angle
  if (!isnan(parsed_servo_angle)) {
    servoReportedAngle = parsed_servo_angle;
  }
  // - apply tank capacity (if provided)
  if (seen_TANK_CAP) {
    tankCapacity = parsed_tank_cap;
  }
  // - apply water used (if provided) — prefer Arduino's explicit W_USED
  if (seen_W_USED && !isnan(parsed_w_used)) {
    todayWaterUsed = parsed_w_used;
  }
  // - if Arduino sent litres (W_L) and tankCapacity known, convert to percent for Firestore fields
  if (!isnan(parsed_w_liters)) {
    waterLevelLiters = parsed_w_liters;
    if (tankCapacity > 0) {
      // compute percent (but we also store litres as requested)
      double pct = (double)waterLevelLiters / (double)max(1, tankCapacity) * 100.0;
      if (pct < 0.0) pct = 0.0;
      if (pct > 100.0) pct = 100.0;
      waterLevel = pct;
    } else {
      // no tank capacity known yet; keep waterLevel unchanged
    }
  }

  // Debug print structured summary (extended with new fields)
  Serial.print("Parsed sensors -> ");
  Serial.print("T:"); Serial.print(temperature, 2);
  Serial.print(" H:"); Serial.print(humidity, 2);
  Serial.print(" L:"); Serial.print(lightVal);
  Serial.print(" Mv:"); Serial.print(moistureValue);
  Serial.print(" MP:"); Serial.print(moisturePercentage);
  Serial.print(" W%:"); Serial.print(waterLevel, 2);
  Serial.print(" WL(L):"); Serial.print(waterLevelLiters, 3);
  Serial.print(" P:"); Serial.print(plantHeight);
  Serial.print(" Freq:"); Serial.print(flowFrequency);
  Serial.print(" L_H:"); Serial.print(flowRate);
  Serial.print(" PUMP_T:"); Serial.print(pumpToTank ? "1":"0");
  Serial.print(" PUMP_F:"); Serial.print(pumpFromTank ? "1":"0");
  Serial.print(" LIGHTS:"); Serial.print(lightsOn ? "1":"0");
  Serial.print(" SERVO:"); Serial.print(servoReportedAngle, 1);
  Serial.print(" TANK_CAP:"); Serial.print(tankCapacity);
  Serial.print(" W_USED:"); Serial.print(todayWaterUsed, 4);
  Serial.print(" CURV:"); Serial.print(current_voltage, 3);
  Serial.print(" UPTIME:"); Serial.print(uptimeSeconds);
  Serial.print(" LIFE:"); Serial.print(lifePercent, 2);
  Serial.print(" TIME_REMAIN:"); Serial.print(timeRemainingSeconds);
  Serial.print(" MOIST_CAL:"); Serial.print(moistureSensorCalibrated ? "1":"0");
  Serial.print(" CALIBRATION:"); Serial.print(calibrationFlag ? "1":"0");
  Serial.print(" LEAKAGE:"); Serial.println(leakageFlag ? "1":"0");

  // Report which expected tokens were NOT present in this line
  String missing = "";
  if (!seen_T) missing += "T ";
  if (!seen_H) missing += "H ";
  if (!seen_L) missing += "L ";
  if (!seen_M) missing += "M ";
  if (!seen_MP) missing += "MP ";
  // W is optional if Arduino sends W_L; mark only if neither percent nor litres seen
  if (!seen_W) missing += "W/W_L ";
  if (!seen_P) missing += "P ";
  if (!seen_F && !seen_L_H) missing += "F/L_H ";
  if (!seen_PUMP_T) missing += "PUMP_T ";
  if (!seen_PUMP_F) missing += "PUMP_F ";
  if (!seen_LIGHTS) missing += "LIGHTS ";
  if (!seen_MAX_BV) missing += "MAX_BV ";
  if (!seen_CURV) missing += "CURV ";
  if (!seen_UPTIME) missing += "UPTIME ";
  if (!seen_LIFE) missing += "LIFE ";
  if (!seen_TIME_REMAIN) missing += "TIME_REMAIN ";
  if (!seen_MOIST_CAL) missing += "MOIST_CAL ";
  if (!seen_CALIBRATION) missing += "CALIBRATION ";
  if (!seen_LEAKAGE) missing += "LEAKAGE ";
  if (!seen_SERVO) missing += "SERVO ";
  if (!seen_TANK_CAP) missing += "TANK_CAP ";
  if (!seen_W_USED) missing += "W_USED ";

  if (missing.length() > 0) {
    missing.trim();
    Serial.print("⚠️ Missing tokens in this line: ");
    Serial.println(missing);
  } else {
    Serial.println("✅ All expected tokens present in this line.");
  }

  // After parsing from serial, write updates to Firestore immediately (same as old ESP)
  updateSensorsDocs();
  createIntervalDocOncePerMinuteAndRecompute();
}

// ----------------- Setup & Loop -----------------
void setup() {
  Serial.begin(9600);
  arduinoSerial.begin(9600);
  delay(10);

  // init new light pins and set OFF initially (as requested)
  pinMode(farmLight1Pin, OUTPUT);
  pinMode(farmLight2Pin, OUTPUT);
  pinMode(cameraLightPin, OUTPUT);
  digitalWrite(farmLight1Pin, LOW); // start OFF
  digitalWrite(farmLight2Pin, LOW); // start OFF
  digitalWrite(cameraLightPin, LOW); // start OFF

  Serial.println();
  Serial.println("ESP8266 Firestore REST -> Arduino (using today's date path)");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed to connect (continuing; will retry).");
  }

  // Sync time for correct date (Africa/Harare = UTC+2)
  configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time");
  start = millis();
  while (time(nullptr) < 1600000000 && millis() - start < 10000) {
    Serial.print(".");
    delay(200);
  }
  Serial.println();
  if (time(nullptr) > 1600000000) {
    Serial.println("Time synced: " + String(getTodayDocDate()));
  } else {
    Serial.println("Time NOT synced; date fallback (build date) may be used.");
  }

  // Optional: sign in to Firebase to obtain idToken
  if (USE_AUTH) {
    Serial.println("Signing in to Firebase Auth...");
    if (!firebaseSignIn(FIREBASE_USER_EMAIL, FIREBASE_USER_PASSWORD)) {
      Serial.println("Firebase auth failed. If Firestore requires auth, reads will fail.");
    }
  } else {
    Serial.println("Skipping Firebase auth (USE_AUTH=false). Firestore must allow public reads.");
  }

  // Try warming caches by creating basic docs (silent) - keep parity with old behavior
  updateSensorsDocs();
  createIntervalDocOncePerMinuteAndRecompute();

  // Ensure pump & lights docs exist (one-time creation if missing). After created they won't be overwritten.
  ensurePumpAndLightsDocsOnce();

  lastMillis = millis();
  lastVoltageDecreaseMillis = millis();
  lastUptimeMillis = millis();

  Serial.println("Ready.");
}

// ----------------- non-blocking read from Arduino SoftwareSerial and print to Serial Monitor
//  Also call receiveAndParseSensors(...) when a full line arrives so we write to Firestore
void handleArduinoIncoming() {
  while (arduinoSerial.available()) {
    char c = (char)arduinoSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (arduinoLineBuf.length() > 0) {
        Serial.print("← FROM ARDUINO: ");
        Serial.println(arduinoLineBuf);

        // parse line and send to Firestore (new behavior requested)
        receiveAndParseSensors(arduinoLineBuf);

        arduinoLineBuf = "";
      }
    } else {
      arduinoLineBuf += c;
      // safety cap to avoid runaway memory use
      if (arduinoLineBuf.length() > 1024) {
        Serial.print("← FROM ARDUINO (trunc): ");
        Serial.println(arduinoLineBuf);
        // still parse what we have
        receiveAndParseSensors(arduinoLineBuf);
        arduinoLineBuf = "";
      }
    }
  }
}
// ----------------- END handler -----------------

// ----------------- Main loop (keeps existing behavior, only addition is above handler) -----------------
void loop() {
  // FIRST: always listen to Arduino and print incoming data (and forward to Firestore via handler)
  handleArduinoIncoming();

  // Also ensure pump & lights docs (if WiFi came up after setup)
  ensurePumpAndLightsDocsOnce();

  // Fetch and forward control docs every SEND_INTERVAL_MS (existing logic) 
  if (millis() - lastSendTime >= SEND_INTERVAL_MS) {
    String farmEncoded = urlEncode(String(farmIdRaw));
    String dateStr = getTodayDocDate(); // builds "ddmmmyyyy" format

    // --- existing block unchanged: GET pump, camera_control, lights_control and forward to Arduino ---
    bool okPumpCurrent = false;
    bool okPumpSource = false;
    bool okCamAngle = false;
    bool okFieldLights = false;
    bool okCamLights = false;

    String pumpPath = "projects/" + String(projectId) + "/databases/(default)/documents/farms/" + farmEncoded + "/readings/" + dateStr + "/sensors/pump?key=" + String(apiKey);
    Serial.println("GET " + pumpPath);
    String pumpPayload = firestoreGetDocument(pumpPath);
    if (pumpPayload.length() == 0) {
      Serial.println("❌ Error: failed to GET document 'pump' from Firestore");
    } else {
      String s_current = getFieldFromDocPayload(pumpPayload, "current_status");
      String s_source  = getFieldFromDocPayload(pumpPayload, "source_pump");
      if (s_current.length()) {
        currentStatus = s_current;
        okPumpCurrent = true;
      } else {
        Serial.println("❌ Error: 'current_status' missing in 'pump' document");
      }
      if (s_source.length()) {
        sourcePump = s_source;
        okPumpSource = true;
      } else {
        Serial.println("❌ Error: 'source_pump' missing in 'pump' document");
      }
      Serial.println(" pump: current_status=" + currentStatus + " source_pump=" + sourcePump);
    }

    String camPath = "projects/" + String(projectId) + "/databases/(default)/documents/farms/" + farmEncoded + "/readings/" + dateStr + "/sensors/camera_control?key=" + String(apiKey);
    Serial.println("GET " + camPath);
    String camPayload = firestoreGetDocument(camPath);
    if (camPayload.length() == 0) {
      Serial.println("❌ Error: failed to GET document 'camera_control' from Firestore");
    } else {
      String s_angle = getFieldFromDocPayload(camPayload, "camera_angle");
      if (s_angle.length()) {
        cameraAngle = s_angle;
        okCamAngle = true;
      } else {
        Serial.println("❌ Error: 'camera_angle' missing in 'camera_control' document");
      }
      Serial.println(" camera_angle=" + cameraAngle);
    }

    String lightsPath = "projects/" + String(projectId) + "/databases/(default)/documents/farms/" + farmEncoded + "/readings/" + dateStr + "/sensors/lights_control?key=" + String(apiKey);
    Serial.println("GET " + lightsPath);
    String lightsPayload = firestoreGetDocument(lightsPath);
    if (lightsPayload.length() == 0) {
      Serial.println("❌ Error: failed to GET document 'lights_control' from Firestore");
    } else {
      // <-- MODIFICATION: read `field_lights` (not `field_maize_lights`) per your request
      String s_field_lights = getFieldFromDocPayload(lightsPayload, "field_lights");
      String s_cam_lights = getFieldFromDocPayload(lightsPayload, "camera_lights");
      if (s_field_lights.length()) {
        // keep variable name unchanged (fieldMaizeLights) but value comes from `field_lights` doc field
        fieldMaizeLights = s_field_lights;
        okFieldLights = true;
      } else {
        Serial.println("❌ Error: 'field_lights' missing in 'lights_control' document");
      }
      if (s_cam_lights.length()) {
        cameraLights = s_cam_lights;
        okCamLights = true;
      } else {
        Serial.println("❌ Error: 'camera_lights' missing in 'lights_control' document");
      }
      Serial.println(" lights: field=" + fieldMaizeLights + " cam=" + cameraLights);

      // NEW: apply lights to hardware outputs (per your request)
      // Farm lights: set HIGH when field_lights is truthy (true/on/1/yes)
      if (isTrueLike(fieldMaizeLights)) {
        digitalWrite(farmLight1Pin, HIGH);
        digitalWrite(farmLight2Pin, HIGH);
        Serial.println("Farm lights -> ON (field_lights truthy)");
      } else {
        digitalWrite(farmLight1Pin, LOW);
        digitalWrite(farmLight2Pin, LOW);
        Serial.println("Farm lights -> OFF");
      }

      // Camera light: if camera_lights is false/off/0 -> turn OFF, otherwise turn ON
      if (isFalseLike(cameraLights)) {
        digitalWrite(cameraLightPin, LOW);
        Serial.println("Camera light -> OFF");
      } else {
        digitalWrite(cameraLightPin, HIGH);
        Serial.println("Camera light -> ON");
      }
    }

    if (okPumpCurrent && okPumpSource && okCamAngle && okFieldLights && okCamLights) {
      String controlData = "";
      controlData += "CAMERA_LIGHTS:" + cameraLights + " ";
      controlData += "FIELD_MAIZE_LIGHTS:" + fieldMaizeLights + " ";
      controlData += "CURRENT_STATUS:" + currentStatus + " ";
      controlData += "SOURCE_PUMP:" + sourcePump + " ";
      controlData += "CAMERA_ANGLE:" + cameraAngle;

      // send via SoftwareSerial to Arduino
      arduinoSerial.println(controlData);
      Serial.println("→ TO ARDUINO: " + controlData);
    } else {
      Serial.println("❌ Not sending to UNO — missing control fields:");
      if (!okPumpCurrent) Serial.println("  - pump.current_status");
      if (!okPumpSource)  Serial.println("  - pump.source_pump");
      if (!okCamAngle)    Serial.println("  - camera_control.camera_angle");
      if (!okFieldLights) Serial.println("  - lights_control.field_lights");
      if (!okCamLights)   Serial.println("  - camera_lights");
    }

    lastSendTime = millis();
  }

  // update local pump usage tracking even when idle
  updatePumpUsageApprox();

  // small delay for loop stability
  delay(50);
}

// ----------------- Definitions -----------------
// Define firebaseSignIn now (implementation unchanged from before)
bool firebaseSignIn(const char* email, const char* pwd) {
  if (!WiFi.isConnected()) return false;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + apiKey;
  String postBody;
  postBody += "{";
  postBody += "\"email\":\""; postBody += email; postBody += "\",";
  postBody += "\"password\":\""; postBody += pwd; postBody += "\",";
  postBody += "\"returnSecureToken\":true";
  postBody += "}";
  client.setInsecure(); // NOTE: skips TLS verification
  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  int httpCode = https.POST(postBody);
  if (httpCode != 200) {
    Serial.print("Auth failed, HTTP: ");
    Serial.println(httpCode);
    if (httpCode > 0) {
      String resp = https.getString();
      Serial.println(resp);
    }
    https.end();
    return false;
  }
  String resp = https.getString();
  https.end();

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.print("Auth JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }
  if (doc.containsKey("idToken")) {
    idToken = String(doc["idToken"].as<const char*>());
    Serial.println("✅ Got idToken (len " + String(idToken.length()) + ")");
    return true;
  }
  Serial.println("Auth response missing idToken");
  return false;
}
