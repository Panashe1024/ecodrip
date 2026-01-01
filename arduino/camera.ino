#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Camera pin definitions for AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// WiFi credentials
const char* ssid = "varoti";
const char* password = "00053278";

// Firestore configuration
const char* projectId = "ecodrip-ddcf0";
const char* apiKey = "AIzaSyC9D1TLbMsc-3iHMGEWaF_YVBTirg6L-Hc"; // Your Web API Key
const char* farmId = "unalytix@gmail.com";

// Server configuration
WebServer server(80);

void setup() {
  Serial.begin(9600);
  Serial.println();
  Serial.println("🔧 ============ ESP32-CAM FIRESTORE SETUP ============");
  
  // Initialize camera
  Serial.println("📷 Initializing camera...");
  if (!setupCamera()) {
    Serial.println("❌ Camera initialization failed!");
    return;
  }
  
  // Connect to WiFi
  Serial.println("📡 Starting WiFi connection...");
  setupWiFi();
  
  // Setup server routes
  Serial.println("🌐 Setting up web server routes...");
  setupServer();
  
  Serial.println("✅ ============ ESP32-CAM SETUP COMPLETED ============");
  Serial.print("🎥 Camera Stream URL: http://");
  Serial.println(WiFi.localIP());
}

bool setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ Camera init failed with error 0x%x\n", err);
    return false;
  }
  
  Serial.println("✅ Camera initialized successfully!");
  return true;
}

void setupWiFi() {
  Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi connected successfully!");
    Serial.print("📱 IP Address: ");
    Serial.println(WiFi.localIP());
    
    // Update Firestore with camera IP
    Serial.println("🔥 Starting Firestore IP update...");
    updateFirestoreIP();
  } else {
    Serial.println("\n❌ Failed to connect to WiFi!");
    WiFi.softAP("ESP32-CAM", "12345678");
    Serial.println("📶 AP Mode Started");
    Serial.print("🔗 AP IP Address: ");
    Serial.println(WiFi.softAPIP());
  }
}

void updateFirestoreIP() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("🔥 Starting Firestore communication...");
    
    HTTPClient http;
    
    // Firestore REST API URL for updating a document
    String url = "https://firestore.googleapis.com/v1/projects/" + String(projectId) + 
                 "/databases/(default)/documents/farms/" + String(farmId) + 
                 "?updateMask.fieldPaths=camera_ip&updateMask.fieldPaths=last_updated&key=" + String(apiKey);
    
    Serial.println("🔗 Firestore URL: " + url);
    
    String ipAddress = WiFi.localIP().toString();
    
    // Firestore document structure
    String jsonData = "{"
      "\"fields\": {"
        "\"camera_ip\": {\"stringValue\": \"" + ipAddress + "\"},"
        "\"last_updated\": {\"integerValue\": \"" + String(millis()) + "\"}"
      "}"
    "}";
    
    Serial.println("📤 Sending data to Firestore: " + jsonData);
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    Serial.println("⏳ Making PATCH request to Firestore...");
    int httpResponseCode = http.PATCH(jsonData);
    
    if (httpResponseCode > 0) {
      Serial.printf("✅ Firestore update successful! Response code: %d\n", httpResponseCode);
      String response = http.getString();
      Serial.println("📥 Firestore response: " + response);
    } else {
      Serial.printf("❌ Error updating Firestore. Response code: %d\n", httpResponseCode);
      Serial.println("🔍 Error details: " + http.errorToString(httpResponseCode));
      
      // Try alternative method - create or update the document
      Serial.println("🔄 Trying alternative method...");
      updateFirestoreAlternative();
    }
    
    http.end();
  } else {
    Serial.println("❌ Cannot update Firestore - WiFi not connected");
  }
}

void updateFirestoreAlternative() {
  Serial.println("🔄 Using alternative Firestore update method...");
  
  HTTPClient http;
  
  // Alternative: Use the documents:create method
  String url = "https://firestore.googleapis.com/v1/projects/" + String(projectId) + 
               "/databases/(default)/documents/farms?documentId=" + String(farmId) + 
               "&key=" + String(apiKey);
  
  Serial.println("🔗 Alternative Firestore URL: " + url);
  
  String ipAddress = WiFi.localIP().toString();
  
  String jsonData = "{"
    "\"fields\": {"
      "\"camera_ip\": {\"stringValue\": \"" + ipAddress + "\"},"
      "\"last_updated\": {\"integerValue\": \"" + String(millis()) + "\"}"
    "}"
  "}";
  
  Serial.println("📤 Sending data (alternative method): " + jsonData);
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  Serial.println("⏳ Making POST request to Firestore...");
  int httpResponseCode = http.POST(jsonData);
  
  if (httpResponseCode > 0) {
    Serial.printf("✅ Alternative method successful! Response code: %d\n", httpResponseCode);
    String response = http.getString();
    Serial.println("📥 Response: " + response);
  } else {
    Serial.printf("❌ Alternative method failed. Response code: %d\n", httpResponseCode);
    Serial.println("🔍 Error: " + http.errorToString(httpResponseCode));
  }
  
  http.end();
}

void setupServer() {
  server.enableCORS(true);
  server.enableDelay(false);

  server.on("/", HTTP_GET, []() {
    Serial.println("🌐 Home page requested");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/html", 
      "<html>"
        "<head><title>ESP32-CAM</title></head>"
        "<body>"
          "<h1>ESP32-CAM Live Stream</h1>"
          "<img src='/stream' style='width:100%; max-width:800px;'/>"
          "<br>"
          "<button onclick='captureImage()'>Capture Photo</button>"
          "<script>"
            "function captureImage() { window.open('/capture', '_blank'); }"
            "setInterval(function() { document.images[0].src = '/stream?' + Date.now(); }, 100);"
          "</script>"
        "</body>"
      "</html>");
    Serial.println("✅ Home page served");
  });

  server.on("/stream", HTTP_GET, []() {
    Serial.println("🎥 Starting video stream...");
    WiFiClient client = server.client();
    
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
    response += "Access-Control-Allow-Origin: *\r\n\r\n";
    server.sendContent(response);

    int frameCount = 0;
    while (client.connected()) {
      camera_fb_t * fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("❌ Camera capture failed");
        break;
      }

      response = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + 
                 String(fb->len) + "\r\n\r\n";
      server.sendContent(response);
      client.write(fb->buf, fb->len);
      server.sendContent("\r\n");
      
      esp_camera_fb_return(fb);
      frameCount++;
      
      if (frameCount % 10 == 0) {
        Serial.printf("📊 Streamed %d frames\n", frameCount);
      }
      
      delay(100);
    }
    Serial.printf("🎬 Stream ended. Total frames: %d\n", frameCount);
  });

  server.on("/capture", HTTP_GET, []() {
    Serial.println("📸 Capture request received");
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("❌ Camera capture failed");
      server.send(500, "text/plain", "Camera capture failed");
      return;
    }

    Serial.printf("✅ Captured image: %d bytes\n", fb->len);
    
    WiFiClient client = server.client();
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: image/jpeg\r\n";
    response += "Content-Length: " + String(fb->len) + "\r\n";
    response += "Access-Control-Allow-Origin: *\r\n\r\n";
    server.sendContent(response);
    client.write(fb->buf, fb->len);
    
    esp_camera_fb_return(fb);
    Serial.println("✅ Snapshot sent successfully");
  });

  server.on("/status", HTTP_GET, []() {
    Serial.println("📊 Status request received");
    String json = "{";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":\"" + String(WiFi.RSSI()) + "\"";
    json += "}";
    
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
    Serial.println("✅ Status sent");
  });

  server.begin();
  Serial.println("✅ HTTP server started");
}

void loop() {
  server.handleClient();
  
  // Update Firestore every 5 minutes
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 300000) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("🔄 5 minutes elapsed - updating Firestore...");
      updateFirestoreIP();
      lastUpdate = millis();
    }
  }
  
  delay(10);
}