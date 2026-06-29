#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h> 
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include "secrets.h" // Pulls in your hidden credentials

// Wi-Fi Credentials (Loaded from secrets.h)
const char* wifiname = SECRET_WIFI_NAME;
const char* wifipassword = SECRET_WIFI_PASS;
const char* mifiname = SECRET_MIFI_NAME;
const char* mifipassword = SECRET_MIFI_PASS;

// HiveMQ Credentials (For receiving remote pump commands)
const char* mqtt_server = "135e2067aa8b40e7b8ee44698f52f249.s1.eu.hivemq.cloud"; 
const int mqtt_port = 8883; 
const char* mqtt_user = SECRET_MQTT_USER;
const char* mqtt_pass = SECRET_MQTT_PASS;
const char* topic_subscribe = "unit1/spraying"; 

// Express Backend URL (For sending sensor data & getting dynamic timer updates)
const String backend_url = "https://backend-beans-farm-pest-disease.onrender.com/sensor/saverawsensordata"; 

// Actuators & Indicators
#define RELAY_1_PIN 18   // D18 - Fungicide Pump (Disease)
#define RELAY_2_PIN 19   // D19 - Pesticide Pump (Pest)
#define RED_LED_PIN 25   // D25 - System Working Light (Heartbeat)
#define GREEN_LED_PIN 26 // D26 - Data Transfer Light

// Sensors
#define DHTPIN 4         // D4
#define RAIN_PIN 32      // D32 (ADC1)
#define SOIL_PIN 34      // D34 (ADC1)
#define LDR_PIN 35       // D35 (ADC1)

#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Function Declarations
void setup_wifi();
void reconnect();
void flashGreenLED();
void callback(char* topic, byte* payload, unsigned int length);
void sendTelemetryHTTP(String payload);

// TIMERS & STATE VARIABLES (Non-Blocking)
unsigned long previousMillisRed = 0;
unsigned long previousMillisTelemetry = 0;
const long redBlinkInterval = 1000;      // Red LED blinks every 1 second
unsigned long telemetryInterval = 15000; // Default 15 seconds (Dynamically updated by backend)
bool redLedState = LOW;

// Network Clients
WiFiClientSecure espClient; 
PubSubClient client(espClient);
WiFiMulti wifiMulti; 

// SETUP FUNCTION (Runs Once)
void setup() {
  Serial.begin(115200);

  // Initialize Actuators and LEDs
  pinMode(RELAY_1_PIN, OUTPUT);
  pinMode(RELAY_2_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  
  // Set Relays to HIGH (Off) initially to prevent dry firing
  digitalWrite(RELAY_1_PIN, HIGH); 
  digitalWrite(RELAY_2_PIN, HIGH); 
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);

  // Initialize Sensors
  dht.begin();
  pinMode(RAIN_PIN, INPUT_PULLUP);
  pinMode(SOIL_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);

  // Start Network Connections
  setup_wifi();

  // Configure MQTT
  espClient.setInsecure(); // Skip certificate validation for prototyping
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); 
}

// NETWORK CONNECTION FUNCTIONS
void setup_wifi() {
  Serial.println("\nInitializing WiFi Scan Matrix...");

  // Register both AP profiles to memory
  wifiMulti.addAP(wifiname, wifipassword);
  wifiMulti.addAP(mifiname, mifipassword);

  Serial.print("Connecting to strongest available network (Phone or MiFi)...");
  
  // Scans and automatically picks the one that is active/stronger
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected successfully!");
  Serial.print("Active SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    // Before attempting MQTT, make sure Wi-Fi didn't drop out completely
    if (wifiMulti.run() != WL_CONNECTED) {
      Serial.println("WiFi lost during MQTT reconnect. Re-verifying network...");
      delay(2000);
      continue;
    }

    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Connected to HiveMQ!");
      client.subscribe(topic_subscribe);
    } else {
      Serial.print("Failed, rc=");
      Serial.print(client.state());
      Serial.println(" trying again in 5 seconds");
      delay(5000);
    }
  }
}

// VISUAL FEEDBACK HELPER
void flashGreenLED() {
  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(150); 
  digitalWrite(GREEN_LED_PIN, LOW);
}

// MQTT CALLBACK (Receives pump commands from Web App)
void callback(char* topic, byte* payload, unsigned int length) {
  flashGreenLED(); 

  String incomingJsonString; 
  
  for (int i = 0; i < length; i++) {
    incomingJsonString += (char)payload[i];
  }
  
  Serial.println("MQTT Received: " + incomingJsonString);

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, incomingJsonString);
  
  if (error) {
    Serial.println("JSON Parse failed.");
    return;
  }

  String status = doc["status"];
  bool action = doc["action"]; 
  
  if (status == "disease") {
    if (action == true) {
      Serial.println("Spraying Fungicide! (Forcing Pesticide OFF for safety)");
      digitalWrite(RELAY_2_PIN, HIGH); 
      delay(50);                       
      digitalWrite(RELAY_1_PIN, LOW);  
    } else {
      Serial.println("Stopping Fungicide.");
      digitalWrite(RELAY_1_PIN, HIGH);
    }
  } 
  else if (status == "pest") {
    if (action == true) {
      Serial.println("Spraying Pesticide! (Forcing Fungicide OFF for safety)");
      digitalWrite(RELAY_1_PIN, HIGH); 
      delay(50);                       
      digitalWrite(RELAY_2_PIN, LOW);  
    } else {
      Serial.println("Stopping Pesticide.");
      digitalWrite(RELAY_2_PIN, HIGH);
    }
  }
}

// HTTP POST (Sends sensor data & receives dynamic timer updates)
void sendTelemetryHTTP(String payload) {
  // wifiMulti.run() automatically checks health and hooks back up if disconnected
  if (wifiMulti.run() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(backend_url); 
    http.addHeader("Content-Type", "application/json");
    Serial.println("Sending HTTP POST to Backend...");
    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0) {
      String responseString = http.getString();
      Serial.println("Backend Response: " + responseString);

      StaticJsonDocument<200> responseDoc;
      DeserializationError error = deserializeJson(responseDoc, responseString);

      // Extract runtime update for telemetry clock if supplied by backend
      if (!error && responseDoc.containsKey("pollingRateMinutes")) {
        long newIntervalSec = responseDoc["pollingRateMinutes"].as<long>() * 60;
        telemetryInterval = newIntervalSec * 1000; 
        Serial.printf("SUCCESS: Timer remotely updated to %ld seconds.\n", newIntervalSec);
      }
    } else {
      Serial.printf("HTTP Error Code: %d\n", httpResponseCode);
    }
    
    http.end(); 
  } else {
    Serial.println("WiFi Disconnected! Skipping telemetry transmission.");
  }
}

// MAIN LOOP
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // Handle incoming MQTT subscriptions

  unsigned long currentMillis = millis();

  // NON-BLOCKING RED LED (System Working Heartbeat)
  if (currentMillis - previousMillisRed >= redBlinkInterval) {
    previousMillisRed = currentMillis;
    redLedState = !redLedState;
    digitalWrite(RED_LED_PIN, redLedState);
  }

  // NON-BLOCKING TELEMETRY (Dynamic HTTP Interval)
  if (currentMillis - previousMillisTelemetry >= telemetryInterval) {
    previousMillisTelemetry = currentMillis;
    
    // Read Sensors
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    int soilRaw = analogRead(SOIL_PIN);
    int ldrRaw = analogRead(LDR_PIN);
    int rainRaw = analogRead(RAIN_PIN);
    
    // Convert Analog to Percentages 
    int soilPct = map(soilRaw, 4095, 0, 0, 100); 
    int ldrPct = map(ldrRaw, 4095, 0, 0, 100);
    int rainIntensityPct = map(rainRaw, 4095, 0, 0, 100);

    // Build JSON Payload
    StaticJsonDocument<256> doc;
    doc["machine_location"] = "unit1"; 
    doc["temperature"] = temp;
    doc["humidity"] = hum;
    doc["soil_moisture"] = soilPct;
    doc["light_level"] = ldrPct; 
    doc["rain_level"] = rainIntensityPct; 
    
    String payload;
    serializeJson(doc, payload);

    flashGreenLED(); // Visual alert right before outputting
    sendTelemetryHTTP(payload); 
  }
}