#include <DHT.h>
#include <Servo.h>

// Pin Definitions
const int trigWaterLevel = 9;
const int echoWaterLevel = 3;
const int trigPlantHeight = 4;
const int echoPlantHeight = 5;
const int pumpFromTank = 6;
const int pumpToTank = 7;
const int lightsPin = 8;
const int lightSensorPin = A0;
const int moistureSensorPin = A1;
const int flowSensorPin = 2;
const int servoPin = 11; // Servo motor on pin 11

// ====== ADDED FOR REQUEST ======
const int buzzerPin = 12; // buzzer pin (added earlier)
float waterLevelLiters = 0.0;       // computed water level in litres (sent to ESP)
float fullTankLiters = 1.5;         // actual full tank litres used for logic
int tankCapacity = 1.5;              // user-requested "full tank" variable to send via serial (set to 15)
// ================================

// DHT11 Sensor
#define DHT_PIN 10
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// Servo motor
Servo cameraServo;
float currentServoAngle = 90.0; // Start at center position (90 degrees)
float targetServoAngle = 90.0;
const float SERVO_SPEED = 1.0; // Degrees per update

// Servo control lock: only move servo after camera_angle command received
bool cameraAngleReceived = false;

// Variables
long waterLevelDistance = 0;
long plantHeightDistance = 0;
long rawPlantHeightDistance = 0; // raw reading from ultrasonic for plant height
const int heightzero = 17;       // new variable per request

int lightSensorValue = 0;
int moistureValue = 0;
int moisturePercentage = 0;
float temperature = 0;
float humidity = 0;

// Flow sensor variables
volatile int flow_frequency = 0;
unsigned int l_hour = 0;
unsigned long currentTime = 0;
unsigned long previousTime = 0;
unsigned long lastSensorRead = 0;
unsigned long cloopTime = 0;
const unsigned long SENSOR_READ_INTERVAL = 2000;

// Control variables
bool moistureSensorCalibrated = false;
bool calibrationFlag = false;
bool leakageFlag = false;
float cameraAngle = 0.5; // Default to center (0.5)
int cameraLightsState = 0;
int fieldMaizeLightsState = 0;

// Pump state tracking - NEW: Track current pump states
bool currentPumpToTankState = false;
bool currentPumpFromTankState = false;
bool lastPumpToTankState = false;
bool lastPumpFromTankState = false;

// Pump-from-tank usage tracking
unsigned long pumpFromTankOnMillis = 0;
float waterUsedToday = 0.0; // accumulated liters used today (updated on OFF transition)

// Timing
unsigned long lastDataSendMillis = 0;
const unsigned long DATA_SEND_INTERVAL = 5000;

// Serial variables
String receivedData = "";

// --- Interrupt Service Routine (ISR) ---
void flow() {
  flow_frequency++;
}

// ---------- Helper functions ----------
long readUltrasonic(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return -1;
  long distance = duration * 0.034 / 2;
  return distance;
}

void readMoistureSensor() {
  moistureValue = analogRead(moistureSensorPin);
  moisturePercentage = map(moistureValue, 0, 1023, 100, 0);
  moisturePercentage = constrain(moisturePercentage, 0, 100);
}

void readDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity = h;
}

// =============================
// 🔹 SERVO MOTOR CONTROL
// =============================
void updateServoPosition() {
  // only move servo if we've received a camera_angle command from ESP
  if (!cameraAngleReceived) return;

  // Convert camera angle (0-1) to servo angle (0-180)
  targetServoAngle = cameraAngle * 180.0;

  // Smoothly move servo towards target position
  if (abs(currentServoAngle - targetServoAngle) > SERVO_SPEED) {
    if (currentServoAngle < targetServoAngle) {
      currentServoAngle += SERVO_SPEED;
    } else {
      currentServoAngle -= SERVO_SPEED;
    }
    cameraServo.write((int)currentServoAngle);
  }
}

// =============================
// 🔹 PUMP CONTROL FUNCTIONS - UPDATED
// =============================
void setPumpToTank(bool state) {
  if (state != currentPumpToTankState) {
    digitalWrite(pumpToTank, state ? HIGH : LOW);
    currentPumpToTankState = state;
    Serial.print("⚙️ PUMP_TO_TANK: ");
    Serial.println(state ? "ON" : "OFF");
  }
}

void setPumpFromTank(bool state) {
  // detect transitions so we can measure usage
  if (state != currentPumpFromTankState) {
    if (state == true) {
      // turning ON: record start time
      pumpFromTankOnMillis = millis();
    } else {
      // turning OFF: compute session usage and accumulate
      unsigned long durationMs = 0;
      if (millis() >= pumpFromTankOnMillis) {
        durationMs = millis() - pumpFromTankOnMillis;
      } else {
        durationMs = 0; // guard, shouldn't happen
      }
      float hours = (float)durationMs / 3600000.0; // convert ms to hours
      // use current l_hour (liters/hour) to compute liters used this session
      float usedLiters = l_hour * hours;
      waterUsedToday += usedLiters;
    }

    digitalWrite(pumpFromTank, state ? HIGH : LOW);
    currentPumpFromTankState = state;
    Serial.print("⚙️ PUMP_FROM_TANK: ");
    Serial.println(state ? "ON" : "OFF");
  }
}

// =============================
// 🔹 SIMPLE SERIAL RECEIVE from ESP
// =============================
void receiveFromESP() {
  if (Serial.available()) {
    receivedData = Serial.readString();
    receivedData.trim();

    Serial.print("📨 FROM ESP: ");
    Serial.println(receivedData);

    // Process the received data
    parseControlFromESP(receivedData);
  }
}

// =============================
// 🔹 SIMPLE PARSING of Control Commands - UPDATED
// =============================
void parseControlFromESP(const String &data) {
  String s = data;
  s.toLowerCase();

  Serial.println("🔍 Processing control commands...");

  // Simple string checks
  if (s.indexOf("moist_cal:1") >= 0) {
    moistureSensorCalibrated = true;
    Serial.println("🌱 MOIST_CAL: ON");
  }
  else if (s.indexOf("moist_cal:0") >= 0) {
    moistureSensorCalibrated = false;
    Serial.println("🌱 MOIST_CAL: OFF");
  }

  if (s.indexOf("pump_cal:1") >= 0) {
    calibrationFlag = true;
    Serial.println("🔧 PUMP_CAL: ON");
  }
  else if (s.indexOf("pump_cal:0") >= 0) {
    calibrationFlag = false;
    Serial.println("🔧 PUMP_CAL: OFF");
  }

  if (s.indexOf("camera_lights:1") >= 0 || s.indexOf("camera_lights:on") >= 0) {
    cameraLightsState = 1;
    Serial.println("📷 CAMERA_LIGHTS: ON");
  }
  else if (s.indexOf("camera_lights:0") >= 0 || s.indexOf("camera_lights:off") >= 0) {
    cameraLightsState = 0;
    Serial.println("📷 CAMERA_LIGHTS: OFF");
  }

  if (s.indexOf("field_maize_lights:1") >= 0 || s.indexOf("field_maize_lights:on") >= 0) {
    fieldMaizeLightsState = 1;
    digitalWrite(lightsPin, HIGH);
    Serial.println("💡 FIELD_LIGHTS: ON");
  }
  else if (s.indexOf("field_maize_lights:0") >= 0 || s.indexOf("field_maize_lights:off") >= 0) {
    fieldMaizeLightsState = 0;
    digitalWrite(lightsPin, LOW);
    Serial.println("💡 FIELD_LIGHTS: OFF");
  }

  // =============================
  // 🔹 UPDATED PUMP CONTROL LOGIC - Turns once only
  // =============================
  if (s.indexOf("current_status:1") >= 0 || s.indexOf("current_status:on") >= 0) {
    setPumpToTank(true); // Turn ON only once
  }
  else if (s.indexOf("current_status:0") >= 0 || s.indexOf("current_status:off") >= 0) {
    setPumpToTank(false); // Turn OFF only once
  }

  // ALWAYS listen for source_pump and act immediately
  if (s.indexOf("source_pump:1") >= 0 || s.indexOf("source_pump:on") >= 0) {
    setPumpFromTank(true); // Turn ON only once
  }
  else if (s.indexOf("source_pump:0") >= 0 || s.indexOf("source_pump:off") >= 0) {
    setPumpFromTank(false); // Turn OFF only once
  }

  // =============================
  // 🔹 CAMERA ANGLE CONTROL - ONLY when provided by ESP
  // =============================
  int angleIndex = s.indexOf("camera_angle:");
  if (angleIndex >= 0) {
    String angleStr = s.substring(angleIndex + 13);
    // Find the end of the angle value (space or end of string)
    int spaceIndex = angleStr.indexOf(' ');
    if (spaceIndex > 0) {
      angleStr = angleStr.substring(0, spaceIndex);
    }

    float newAngle = angleStr.toFloat();
    // Validate angle range (0-1)
    if (newAngle < 0.0) newAngle = 0.0;
    if (newAngle > 1.0) newAngle = 1.0;

    cameraAngle = newAngle; // update target; updateServoPosition will move servo slowly
    cameraAngleReceived = true; // allow servo to move from now on
    Serial.print("📐 CAMERA_ANGLE: ");
    Serial.print(cameraAngle);

    // Print direction information
    if (cameraAngle < 0.5) {
      Serial.println(" (LEFT direction)");
    } else if (cameraAngle > 0.5) {
      Serial.println(" (RIGHT direction)");
    } else {
      Serial.println(" (CENTER position)");
    }
  }

  Serial.println("✅ Command processing complete");
}

// =============================
// 🔹 PRINT PUMP STATUS - NEW: Shows current pump states
// =============================
void printPumpStatus() {
  static unsigned long lastStatusPrint = 0;

  if (millis() - lastStatusPrint > 3000) { // Print every 3 seconds
    lastStatusPrint = millis();

    Serial.print("🔧 PUMP STATUS - ToTank: ");
    Serial.print(currentPumpToTankState ? "ON" : "OFF");
    Serial.print(" | FromTank: ");
    Serial.println(currentPumpFromTankState ? "ON" : "OFF");
  }
}

// =============================
// 🔹 SEND SENSOR DATA to ESP (now sent continuously each loop)
// =============================
void sendSensorDataToESP() {
  // compute current session usage if pump currently on
  float currentSessionUsed = 0.0;
  if (currentPumpFromTankState && pumpFromTankOnMillis > 0) {
    unsigned long durationMs = 0;
    if (millis() >= pumpFromTankOnMillis) {
      durationMs = millis() - pumpFromTankOnMillis;
    }
    float hours = (float)durationMs / 3600000.0;
    currentSessionUsed = l_hour * hours;
  }

  float totalUsedToday = waterUsedToday + currentSessionUsed;

  String data = "";

  data += "T:" + String(temperature, 2) + " ";
  data += "H:" + String(humidity, 2) + " ";
  data += "L:" + String(lightSensorValue) + " ";
  data += "M:" + String(moistureValue) + " ";
  data += "MP:" + String(moisturePercentage) + " ";
  // send water level in litres (actual) instead of raw distance:
  data += "W_L:" + String(waterLevelLiters, 3) + " "; // litres with 3 decimal places

  // send recalculated plant height (heightzero - rawPlantHeightDistance)
  data += "P:" + String(plantHeightDistance) + " ";

  data += "F:" + String(flow_frequency) + " ";
  data += "L_H:" + String(l_hour) + " ";
  data += "PUMP_T:" + String(currentPumpToTankState ? "1" : "0") + " ";
  data += "PUMP_F:" + String(currentPumpFromTankState ? "1" : "0") + " ";
  data += "LIGHTS:" + String(digitalRead(lightsPin) == HIGH ? "1" : "0") + " ";
  data += "SERVO:" + String(currentServoAngle, 1) + " ";
  data += "TANK_CAP:" + String(tankCapacity) + " ";
  data += "W_USED:" + String(totalUsedToday, 4); // water used today (liters), includes ongoing session

  Serial.println(data);
}

// --- Setup & Loop ---
void setup() {
  Serial.begin(9600);

  // Initialize pins
  pinMode(trigWaterLevel, OUTPUT);
  pinMode(echoWaterLevel, INPUT);
  pinMode(trigPlantHeight, OUTPUT);
  pinMode(echoPlantHeight, INPUT);
  pinMode(pumpFromTank, OUTPUT);
  pinMode(pumpToTank, OUTPUT);
  pinMode(lightsPin, OUTPUT);
  pinMode(lightSensorPin, INPUT);
  pinMode(moistureSensorPin, INPUT);
  pinMode(flowSensorPin, INPUT);

  // ====== ADDED ======
  pinMode(buzzerPin, OUTPUT); // buzzer
  digitalWrite(buzzerPin, LOW);
  // ====================

  // Initialize DHT sensor
  dht.begin();

  // Initialize servo motor
  cameraServo.attach(servoPin);

  // Ensure servo starts at cameraAngle == 0.5 (midpoint) and lock movement until command
  currentServoAngle = cameraAngle * 180.0; // 0.5 * 180 = 90 deg
  targetServoAngle = currentServoAngle;
  cameraServo.write((int)currentServoAngle);
  cameraAngleReceived = false; // will only move after ESP command
  Serial.println("📐 Servo motor initialized at center position (90°)");

  // Attach interrupt for flow sensor
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), flow, RISING);

  currentTime = millis();
  cloopTime = currentTime;

  // Initialize pumps OFF and lights ON
  setPumpToTank(false);
  setPumpFromTank(false);
  digitalWrite(lightsPin, HIGH);

  Serial.println("✅ Arduino Started - Listening for ESP...");
  Serial.println("📐 Servo ready - Camera angle range: 0.0 (0°) to 1.0 (180°)");
  Serial.println("⚙️ Pump control: Turns ONCE when command received");
  Serial.println("👂 Always listening for pump status changes");
}

void loop() {
  currentTime = millis();

  // Check for incoming data from ESP - CONTINUOUS LISTENING
  receiveFromESP();

  // Update servo position smoothly (only if cameraAngleReceived)
  updateServoPosition();

  // Print current pump status
  printPumpStatus();

  // --- Flow Sensor Calculation Every 1 Second ---
  if (currentTime >= (cloopTime + 1000)) {
    cloopTime = currentTime;
    l_hour = (flow_frequency * 60) / 7.5;
    flow_frequency = 0;
  }

  // --- Read sensors every 500ms ---
  if (currentTime - previousTime >= 500) {
    previousTime = currentTime;

    waterLevelDistance = readUltrasonic(trigWaterLevel, echoWaterLevel);

    // read raw plant height then recalculate as (heightzero - raw)
    rawPlantHeightDistance = readUltrasonic(trigPlantHeight, echoPlantHeight);
    if (rawPlantHeightDistance <= 0) {
      plantHeightDistance = 0;
    } else {
      long diff = (long)heightzero - rawPlantHeightDistance;
      if (diff < 0) diff = 0;
      plantHeightDistance = diff;
    }

    lightSensorValue = analogRead(lightSensorPin);
    readMoistureSensor();

    // ====== NEW: compute water level in litres based on distance ratio ======
    // mapping:
    //   distance = 2 cm -> full (fullTankLiters)
    //   distance = 12 cm -> empty (0 L)
    // linear interpolation:
    const float minDistance = 2.0;   // distance when tank is full
    const float maxDistance = 12.0;  // distance when tank is empty
    if (waterLevelDistance <= 0) {
      // out-of-range reading (pulseIn timed out)
      waterLevelLiters = 0.0;
    } else {
      float d = (float)waterLevelDistance;
      // clamp distance to [minDistance, maxDistance]
      if (d < minDistance) d = minDistance;
      if (d > maxDistance) d = maxDistance;
      float ratio = (maxDistance - d) / (maxDistance - minDistance); // 0..1
      waterLevelLiters = ratio * fullTankLiters;
    }

    // ====== NEW: buzzer & pumpFromTank control based on water level ======
    bool isEmpty = (waterLevelDistance >= (long)maxDistance) || (waterLevelLiters <= 0.005);
    bool isFull = (waterLevelDistance <= (long)minDistance) || (waterLevelLiters >= fullTankLiters - 0.0001);

    if (isEmpty) {
      digitalWrite(buzzerPin, HIGH); // sound buzzer when empty
      // pumpFromTank unchanged here (we don't force pump)
    } else if (isFull) {
      digitalWrite(buzzerPin, HIGH);        // sound buzzer when full
      setPumpFromTank(false);               // turn pumpFromTank LOW when full
    } else {
      digitalWrite(buzzerPin, LOW); // normal (no buzzer)
    }
    // =======================================================================
  }

  // --- Read DHT sensor every 2s ---
  if (currentTime - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = currentTime;
    readDHT();
  }

  // --- SEND sensor data continuously (every loop) ---
  sendSensorDataToESP();

  delay(50); // Small delay for stability
}
