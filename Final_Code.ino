#include <HardwareSerial.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <EEPROM.h>

// DHT22 sensor setup
#define DHT_PIN 27          
#define DHT_TYPE DHT22      
DHT dht(DHT_PIN, DHT_TYPE);

// GSM module setup
HardwareSerial gsmSerial(2);
#define GSM_TX_PIN 17        
#define GSM_RX_PIN 16        

// MQ-135 Air Quality Sensor - DIRECT CONNECTION (no voltage divider)
#define MQ135_PIN 32         

// WiFi credentials - UPDATE THESE
const char* ssid = "TP-LINK_1846";          
const char* password = "22493989";
const char* hostname = "firedetection";

// Web server
WebServer server(80);

// MQ-135 thresholds - Configurable via web interface
int AIR_QUALITY_THRESHOLD = 1200;    // Can be modified via settings
#define NORMAL_BASELINE 500           
#define MAX_EXPECTED 2200             

String phoneNumber = "+9779863797105"; // CONFIGURABLE VIA WEB INTERFACE

// Data logging structure
struct SensorReading {
  unsigned long timestamp;
  float temperature;
  float humidity;
  int airQuality;
  bool alertTriggered;
};

// Data logging variables
SensorReading readings[100];  // Store last 100 readings
int readingIndex = 0;
unsigned long lastLogTime = 0;
const unsigned long logInterval = 30000; // Log every 30 seconds

// Variables for sensor readings
float temperature = 0.0;
float humidity = 0.0;
int airQualityRaw = 0;
int airQualityPercent = 0;
float airQualityVoltage = 0.0;
String airQualityStatus = "UNKNOWN";

// Calibration variables
float tempOffset = 0.0;      // Temperature correction factor
float humidityOffset = 0.0;  // Humidity correction factor

// Alert management with FIXED timing
bool alertSent = false;       
unsigned long lastAlertTime = 0;
const unsigned long alertCooldown = 300000; // 5 minutes
const unsigned long MINIMUM_ALERT_DURATION = 30000; // 30 seconds minimum before reset
unsigned long sensorStartTime = 0;
const unsigned long preheatTime = 120000; // 2 minutes

// Skip warming functionality
bool skipWarmingRequested = false;

// SMS Status tracking variables
String smsStatus = "Ready";
String lastSMSTime = "Never";
int totalSMSSent = 0;
String alertReason = "Normal";

// Timing variables
unsigned long lastSensorRead = 0;
unsigned long sensorInterval = 2000;

// Enhanced GSM power management with SMS state tracking
bool gsmPowerState = false;
unsigned long gsmCooldownTime = 0;
const unsigned long GSM_COOLDOWN_PERIOD = 5000;

// SMS State Management Variables
bool smsInProgress = false;
unsigned long smsStartTime = 0;
const unsigned long SMS_TIMEOUT = 45000; // 45 seconds max
String pendingSMSMessage = "";
bool smsPriorityMode = false;

// WiFi-GSM Coordination Variables
bool gsmOperationInProgress = false;
bool wifiWasConnected = false;
unsigned long wifiReconnectAttempts = 0;

// EEPROM addresses - UPDATED for phone number storage
#define EEPROM_THRESHOLD_ADDR 0
#define EEPROM_TEMP_OFFSET_ADDR 4
#define EEPROM_HUMIDITY_OFFSET_ADDR 8
#define EEPROM_PHONE_NUMBER_ADDR 16  // 50 bytes reserved for phone number
#define EEPROM_SIZE 512

void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.commit(); // Clear any previous initialization issues
  
  // Load saved settings from EEPROM
  loadSettings();
  
  // Initialize sensors
  dht.begin();
  pinMode(MQ135_PIN, INPUT);
  
  // Set ADC attenuation for maximum range (up to 2.45V)
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);
  
  sensorStartTime = millis();
  
  Serial.println("=== Forest Fire Detection & Early Warning System ===");
  Serial.println("🔥 Features: Fixed SMS | WiFi-GSM Coordination | Robust Delivery | Configurable Phone");
  Serial.println("📱 SMS Memory Management: Active");
  Serial.println("🌐 WiFi-GSM Coordination: Enabled");
  Serial.println("🚨 Fixed Alert Timing: Active");
  Serial.println("🔧 Sensor Calibration: Available");
  Serial.println("📞 Configurable Phone Number: Available");
  Serial.println("⚡ Skip Warming Function: Available");
  
  // Initialize WiFi
  initializeWiFiWithMDNS();
  
  // Setup web server
  setupWebServer();
  
  // Initialize GSM with delay
  delay(5000);
  initializeGSMWithPowerManagement();
  
  Serial.println("🚀 Forest Fire Detection System Ready!");
  Serial.println("Commands: 't'=test SMS, 'r'=read sensors, 'g'=GSM test, 'd'=diagnostics");
  Serial.print("🌐 Dashboard: http://");
  Serial.print(hostname);
  Serial.println(".local");
  Serial.print("📱 Mobile access: http://");
  Serial.println(WiFi.localIP());
  Serial.print("📞 Current phone number: ");
  Serial.println(phoneNumber);
  Serial.println("======================================================");
}

void loop() {
  // Debug SMS protection status every 10 seconds
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 10000) {
    Serial.print("🔒 SMS Protection: ");
    Serial.print(smsInProgress ? "ACTIVE" : "INACTIVE");
    Serial.print(" | Alert Status: ");
    Serial.print(alertSent ? "ACTIVE" : "INACTIVE");
    Serial.print(" | Time since alert: ");
    Serial.print((millis() - lastAlertTime) / 1000);
    Serial.println("s");
    lastDebugTime = millis();
  }
  
  // Handle web server ONLY when GSM is not in critical operation
  if (!gsmOperationInProgress && !smsInProgress) {
    server.handleClient();
  }
  
  // Read sensors
  if (millis() - lastSensorRead >= sensorInterval) {
    readSensors();
    displaySensorData();
    checkAlertConditionWithFixedTiming();  // FIXED FUNCTION
    lastSensorRead = millis();
  }
  
  // Handle SMS timeout protection
  handleSMSTimeout();
  
  // Log data periodically
  if (millis() - lastLogTime >= logInterval) {
    logReading();
    lastLogTime = millis();
  }
  
  // Handle commands
  if (Serial.available()) {
    char command = Serial.read();
    handleSerialCommands(command);
  }
  
  // Handle GSM data
  if (gsmPowerState && gsmSerial.available()) {
    String response = gsmSerial.readString();
    Serial.print("GSM: ");
    Serial.println(response);
  }
}

// FIXED: Enhanced settings loading function with proper phone number handling
void loadSettings() {
  int savedThreshold;
  float savedTempOffset, savedHumidityOffset;
  char savedPhoneNumber[20] = {0}; // Initialize with zeros
  
  EEPROM.get(EEPROM_THRESHOLD_ADDR, savedThreshold);
  EEPROM.get(EEPROM_TEMP_OFFSET_ADDR, savedTempOffset);
  EEPROM.get(EEPROM_HUMIDITY_OFFSET_ADDR, savedHumidityOffset);
  
  // Read phone number with proper termination
  for(int i = 0; i < 19; i++) {
    savedPhoneNumber[i] = EEPROM.read(EEPROM_PHONE_NUMBER_ADDR + i);
    if(savedPhoneNumber[i] == '\0') break;
  }
  savedPhoneNumber[19] = '\0'; // Ensure termination
  
  // Validate saved threshold
  if (savedThreshold >= 500 && savedThreshold <= 3000) {
    AIR_QUALITY_THRESHOLD = savedThreshold;
    Serial.print("📁 Loaded threshold from EEPROM: ");
    Serial.println(AIR_QUALITY_THRESHOLD);
  } else {
    Serial.println("📁 Using default threshold: 1200");
  }
  
  // Validate temperature offset
  if (savedTempOffset >= -10.0 && savedTempOffset <= 10.0) {
    tempOffset = savedTempOffset;
    Serial.print("🌡️ Loaded temperature offset: ");
    Serial.println(tempOffset);
  } else {
    Serial.println("🌡️ Using default temperature offset: 0.0");
  }
  
  // Validate humidity offset
  if (savedHumidityOffset >= -20.0 && savedHumidityOffset <= 20.0) {
    humidityOffset = savedHumidityOffset;
    Serial.print("💧 Loaded humidity offset: ");
    Serial.println(humidityOffset);
  } else {
    Serial.println("💧 Using default humidity offset: 0.0");
  }
  
  // Validate phone number
  if (strlen(savedPhoneNumber) >= 10 && savedPhoneNumber[0] == '+') {
    phoneNumber = String(savedPhoneNumber);
    Serial.print("📞 Loaded phone number: ");
    Serial.println(phoneNumber);
  } else {
    Serial.println("📞 Using default phone number");
  }
}

// FIXED: Enhanced settings saving function with proper phone number clearing
void saveSettings() {
  EEPROM.put(EEPROM_THRESHOLD_ADDR, AIR_QUALITY_THRESHOLD);
  EEPROM.put(EEPROM_TEMP_OFFSET_ADDR, tempOffset);
  EEPROM.put(EEPROM_HUMIDITY_OFFSET_ADDR, humidityOffset);
  
  // Save phone number with proper clearing
  char phoneNumberArray[20] = {0}; // Initialize with zeros
  phoneNumber.toCharArray(phoneNumberArray, 19); // Leave space for null terminator
  
  // Clear the EEPROM area first
  for(int i = 0; i < 20; i++) {
    EEPROM.write(EEPROM_PHONE_NUMBER_ADDR + i, 0);
  }
  
  // Write each character individually
  for(int i = 0; i < 19; i++) {
    EEPROM.write(EEPROM_PHONE_NUMBER_ADDR + i, phoneNumberArray[i]);
    if(phoneNumberArray[i] == '\0') break;
  }
  
  EEPROM.commit();
  
  Serial.println("💾 All settings saved to EEPROM:");
  Serial.print("   Threshold: ");
  Serial.println(AIR_QUALITY_THRESHOLD);
  Serial.print("   Temperature offset: ");
  Serial.println(tempOffset);
  Serial.print("   Humidity offset: ");
  Serial.println(humidityOffset);
  Serial.print("   Phone number: ");
  Serial.println(phoneNumber);
}

// FIXED: Enhanced Alert Condition Check with improved timing and debugging
void checkAlertConditionWithFixedTiming() {
  bool isPreheating = (millis() - sensorStartTime) < preheatTime;
  unsigned long timeSinceLastAlert = millis() - lastAlertTime;
  
  // CRITICAL: Check if SMS is in progress first
  if (smsInProgress) {
    Serial.println("📤 SMS TRANSMISSION IN PROGRESS - PROTECTING FROM INTERRUPTION");
    updateAlertReasonForSMSInProgress();
    return; // Don't process any other alerts while SMS is sending
  }
  
  // Enhanced debug output with all critical variables
  Serial.print("🔥 DETAILED DEBUG: Raw=");
  Serial.print(airQualityRaw);
  Serial.print(" | Threshold=");
  Serial.print(AIR_QUALITY_THRESHOLD);
  Serial.print(" | Voltage=");
  Serial.print(airQualityVoltage, 2);
  Serial.print("V | Preheating=");
  Serial.print(isPreheating ? "YES" : "NO");
  Serial.print(" | AlertSent=");
  Serial.print(alertSent ? "YES" : "NO");
  Serial.print(" | TimeSinceAlert=");
  Serial.print(timeSinceLastAlert / 1000);
  Serial.print("s | Phone=");
  Serial.println(phoneNumber);
  
  // Update alert reason for web display with voltage information
  if (isPreheating) {
    alertReason = "Preheating (" + String((preheatTime - (millis() - sensorStartTime)) / 1000) + "s left)";
    smsStatus = "Preheating";
  } else if (airQualityRaw <= AIR_QUALITY_THRESHOLD) {
    int needed = max(0, AIR_QUALITY_THRESHOLD - airQualityRaw);
    alertReason = "Safe levels (need " + String(needed) + " more) | " + String(airQualityVoltage, 2) + "V";
    smsStatus = "Ready";
  } else if (alertSent) {
    if (timeSinceLastAlert < MINIMUM_ALERT_DURATION) {
      alertReason = "Alert active - minimum duration (" + String((MINIMUM_ALERT_DURATION - timeSinceLastAlert) / 1000) + "s left)";
      smsStatus = "Alert Active";
    } else {
      alertReason = "Cooldown active (" + String((alertCooldown - timeSinceLastAlert) / 1000) + "s left)";
      smsStatus = "Cooldown";
    }
  } else {
    alertReason = "FIRE RISK DETECTED - preparing alert | " + String(airQualityVoltage, 2) + "V";
    smsStatus = "Alert Pending";
  }
  
  // FIXED: Alert triggering logic with immediate flag setting
  if (!isPreheating && airQualityRaw > AIR_QUALITY_THRESHOLD && !alertSent) {
    if (timeSinceLastAlert >= alertCooldown) {
      Serial.println("🚨 *** FIRE CONDITION MET - TRIGGERING ALERT ***");
      
      // CRITICAL: Set alert flag BEFORE any SMS operations
      alertSent = true;
      lastAlertTime = millis();
      
      // Log the exact moment of alert triggering
      Serial.print("📊 Alert triggered at sensor value: ");
      Serial.print(airQualityRaw);
      Serial.print(" (threshold: ");
      Serial.print(AIR_QUALITY_THRESHOLD);
      Serial.println(")");
      
      String alertMessage = createEnhancedFireAlertMessage();
      
      if (sendSMSWithFullProtection(alertMessage, "AUTOMATIC_FIRE_ALERT")) {
        totalSMSSent++;
        updateSMSStatus("Fire Alert Sent", "Automatic Detection");
        Serial.println("✅ *** AUTOMATIC FIRE ALERT SENT SUCCESSFULLY ***");
      } else {
        updateSMSStatus("SMS Failed", "Transmission Error");
        Serial.println("❌ *** SMS FAILED BUT ALERT FLAG REMAINS SET ***");
        // Keep alertSent = true to prevent immediate retry
      }
    }
  }
  
  // ENHANCED: More conservative reset logic
  if (!smsInProgress && alertSent && timeSinceLastAlert > MINIMUM_ALERT_DURATION) {
    int resetThreshold = AIR_QUALITY_THRESHOLD * 0.6; // Even more conservative
    if (airQualityRaw < resetThreshold) {
      alertSent = false;
      Serial.print("✅ *** ALERT RESET - Value dropped to ");
      Serial.print(airQualityRaw);
      Serial.print(" (reset threshold: ");
      Serial.print(resetThreshold);
      Serial.println(") ***");
    }
  }
}

// Enhanced SMS Message Creation
String createEnhancedFireAlertMessage() {
  String msg = "🚨 FOREST FIRE ALERT!\n";
  msg += "Location: Forest Detection Unit\n";
  msg += "Risk Level: " + String(airQualityPercent) + "%\n";
  msg += "Temp: " + String(temperature, 1) + "°C\n";
  msg += "Humidity: " + String(humidity, 1) + "%\n";
  msg += "Voltage: " + String(airQualityVoltage, 2) + "V\n";
  msg += "Time: " + String(millis() / 1000) + "s\n";
  msg += "Immediate Action Required!";
  return msg;
}

// Rest of the SMS functions...
void handleSMSTimeout() {
  if (smsInProgress && (millis() - smsStartTime > SMS_TIMEOUT)) {
    Serial.println("⏰ SMS TIMEOUT - RESETTING STATE");
    smsInProgress = false;
    smsPriorityMode = false;
    smsStatus = "Timeout Error";
    alertReason = "SMS timeout - please retry";
    
    if (gsmOperationInProgress) {
      restoreWiFiOperation();
    }
  }
}

void updateAlertReasonForSMSInProgress() {
  int remainingTime = (SMS_TIMEOUT - (millis() - smsStartTime)) / 1000;
  alertReason = "SMS SENDING - DO NOT INTERRUPT (" + String(remainingTime) + "s) | " + String(airQualityVoltage, 2) + "V";
  smsStatus = "Transmitting";
}

bool sendSMSWithFullProtection(String message, String alertType) {
  Serial.println("🚨 STARTING PROTECTED SMS TRANSMISSION");
  Serial.println("🔒 SMS PROTECTION ACTIVE - PREVENTING INTERRUPTION");
  
  smsInProgress = true;
  smsStartTime = millis();
  smsPriorityMode = true;
  pendingSMSMessage = message;
  
  prepareForGSMOperation();
  
  if (!performEnhancedSMSMemoryManagement()) {
    Serial.println("❌ SMS Memory management failed");
    cleanupSMSOperation();
    return false;
  }
  
  bool smsResult = sendSMSWithDeliveryConfirmation(message);
  cleanupSMSOperation();
  
  return smsResult;
}

bool performEnhancedSMSMemoryManagement() {
  Serial.println("🗑️ PERFORMING ENHANCED SMS MEMORY MANAGEMENT");
  
  if (!clearAllSMSMemory()) {
    Serial.println("❌ Failed to clear SMS memory");
    return false;
  }
  
  if (!checkSMSMemorySpace()) {
    Serial.println("❌ Insufficient SMS memory space");
    return false;
  }
  
  if (!configureSMSSettings()) {
    Serial.println("❌ Failed to configure SMS settings");
    return false;
  }
  
  return true;
}

bool clearAllSMSMemory() {
  Serial.println("🗑️ Clearing all SMS memory...");
  
  if (!sendATCommandWithResponse("AT+CMGD=1,4", "OK", 5000)) {
    return false;
  }
  delay(1000);
  
  if (!sendATCommandWithResponse("AT+CPMS=\"SM\",\"SM\",\"SM\"", "OK", 3000)) {
    return false;
  }
  
  return true;
}

bool checkSMSMemorySpace() {
  Serial.println("📊 Checking SMS memory space...");
  
  gsmSerial.println("AT+CPMS?");
  String response = waitForGSMResponse("+CPMS:", 3000);
  
  if (response.indexOf("+CPMS:") >= 0) {
    Serial.println("✅ SMS Memory check passed");
    return true;
  }
  
  Serial.println("⚠️ SMS Memory check uncertain - proceeding");
  return true;
}

bool configureSMSSettings() {
  Serial.println("⚙️ Configuring optimal SMS settings...");
  
  if (!sendATCommandWithResponse("AT+CMGF=1", "OK", 2000)) {
    return false;
  }
  
  if (!sendATCommandWithResponse("AT+CSCS=\"GSM\"", "OK", 2000)) {
    return false;
  }
  
  return true;
}

bool sendSMSWithDeliveryConfirmation(String message) {
  Serial.println("📤 SENDING SMS WITH DELIVERY CONFIRMATION");
  
  String smsCmd = "AT+CMGS=\"" + phoneNumber + "\"";
  gsmSerial.println(smsCmd);
  
  if (!waitForGSMResponse(">", 10000)) {
    Serial.println("❌ No SMS prompt received");
    return false;
  }
  
  Serial.println("✍️ Sending message content...");
  gsmSerial.print(message);
  delay(500);
  gsmSerial.write(26); // Ctrl+Z
  
  Serial.println("⏳ Waiting for delivery confirmation...");
  String response = waitForGSMResponse("+CMGS:", 30000);
  
  if (response.length() > 0 && response.indexOf("+CMGS:") >= 0) {
    Serial.println("✅ SMS DELIVERY CONFIRMED!");
    return true;
  } else {
    Serial.println("❌ SMS delivery failed or timeout");
    return false;
  }
}

void prepareForGSMOperation() {
  gsmOperationInProgress = true;
  wifiWasConnected = (WiFi.status() == WL_CONNECTED);
  
  if (wifiWasConnected) {
    Serial.println("📡 Temporarily disconnecting WiFi for GSM priority");
    WiFi.disconnect(false);
    delay(1000);
  }
  
  if (!checkGSMPowerStatus()) {
    Serial.println("📱 Activating GSM with priority");
  }
}

void restoreWiFiOperation() {
  if (wifiWasConnected) {
    Serial.println("🌐 Restoring WiFi connection...");
    WiFi.begin(ssid, password);
    
    unsigned long wifiStartTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStartTime < 15000) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ WiFi reconnected successfully");
    } else {
      Serial.println("\n⚠️ WiFi reconnection delayed - will retry");
    }
  }
  
  gsmOperationInProgress = false;
}

void cleanupSMSOperation() {
  smsInProgress = false;
  smsPriorityMode = false;
  pendingSMSMessage = "";
  
  restoreWiFiOperation();
  
  Serial.println("🔓 SMS PROTECTION RELEASED");
}

bool sendATCommandWithResponse(String command, String expectedResponse, int timeout) {
  if (!checkGSMPowerStatus()) return false;
  
  Serial.print("AT: ");
  Serial.println(command);
  
  gsmSerial.println(command);
  return waitForGSMResponse(expectedResponse, timeout).indexOf(expectedResponse) >= 0;
}

String waitForGSMResponse(String expectedResponse, int timeout) {
  unsigned long startTime = millis();
  String response = "";
  
  while (millis() - startTime < timeout) {
    if (gsmSerial.available()) {
      char c = gsmSerial.read();
      response += c;
      Serial.write(c);
      
      if (response.indexOf(expectedResponse) >= 0) {
        return response;
      }
      
      if (response.indexOf("ERROR") >= 0) {
        Serial.println("❌ AT command error received");
        return "";
      }
    }
    yield();
  }
  
  Serial.println("⏰ Timeout waiting for: " + expectedResponse);
  return "";
}

String createFireAlertMessage() {
  return "🚨 FOREST FIRE ALERT!\nRisk Level: " + String(airQualityPercent) + 
         "%\nTemp: " + String(temperature, 0) + "C\nHumidity: " + 
         String(humidity, 0) + "%\nVoltage: " + String(airQualityVoltage, 2) + "V\nImmediate Action Required!";
}

void handleSerialCommands(char command) {
  switch(command) {
    case 't': case 'T': 
      if (!smsInProgress) {
        sendTestSMSWithProtection();
      } else {
        Serial.println("⚠️ SMS in progress - please wait");
      }
      break;
    case 'r': case 'R': 
      readSensors(); 
      displaySensorData(); 
      break;
    case 'g': case 'G': 
      if (!smsInProgress) {
        testGSMConnection();
      } else {
        Serial.println("⚠️ SMS in progress - please wait");
      }
      break;
    case 'c': case 'C': 
      calibrateSensors(); 
      break;
    case 'd': case 'D':
      performCompleteSMSDiagnostics();
      break;
    case 's': case 'S':
      displaySystemStatus();
      break;
  }
}

void sendTestSMSWithProtection() {
  String testMessage = "🧪 FIRE DETECTION TEST\nSystem: Forest Fire Detection & Early Warning\nRisk: " + String(airQualityPercent) + 
                      "%\nT:" + String(temperature, 0) + "C H:" +
                      String(humidity, 0) + "%\nVoltage: " + String(airQualityVoltage, 2) + "V\nProtected SMS: Active";
  
  if (sendSMSWithFullProtection(testMessage, "TEST")) {
    totalSMSSent++;
    updateSMSStatus("Test Sent", "Manual Test");
    Serial.println("✅ Protected test SMS sent successfully");
  } else {
    updateSMSStatus("Test Failed", "SMS Error");
    Serial.println("❌ Protected test SMS failed");
  }
}

void performCompleteSMSDiagnostics() {
  Serial.println("📋 === COMPLETE SMS DIAGNOSTICS ===");
  
  sendATCommandWithResponse("AT+CREG?", "OK", 2000);
  delay(1000);
  
  sendATCommandWithResponse("AT+CSQ", "OK", 2000);
  delay(1000);
  
  sendATCommandWithResponse("AT+CSCA?", "OK", 2000);
  delay(1000);
  
  sendATCommandWithResponse("AT+CPMS?", "OK", 2000);
  delay(1000);
  
  sendATCommandWithResponse("AT+CMGD=1,4", "OK", 3000);
  delay(1000);
  
  Serial.println("📞 Checking balance...");
  sendATCommandWithResponse("AT+CUSD=1,\"*101#\"", "OK", 10000);
  delay(3000);
  
  Serial.println("📋 === DIAGNOSTICS COMPLETE ===");
}

void displaySystemStatus() {
  Serial.println("📊 === FOREST FIRE DETECTION SYSTEM STATUS ===");
  Serial.print("SMS Protection: ");
  Serial.println(smsInProgress ? "ACTIVE" : "READY");
  Serial.print("WiFi-GSM Coordination: ");
  Serial.println(gsmOperationInProgress ? "GSM PRIORITY" : "NORMAL");
  Serial.print("Memory Usage: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes free");
  Serial.print("Alert Status: ");
  Serial.println(alertSent ? "ACTIVE" : "READY");
  Serial.print("Time since last alert: ");
  Serial.print((millis() - lastAlertTime) / 1000);
  Serial.println(" seconds");
  Serial.print("Phone Number: ");
  Serial.println(phoneNumber);
  Serial.println("================================");
}

void logReading() {
  readings[readingIndex] = {
    millis(), 
    temperature, 
    humidity, 
    airQualityRaw, 
    alertSent
  };
  readingIndex = (readingIndex + 1) % 100;
}

String getConnectionQuality() {
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    if (rssi > -50) return "Excellent";
    if (rssi > -60) return "Good";
    if (rssi > -70) return "Fair";
    return "Weak";
  }
  return "Disconnected";
}

void initializeWiFiWithMDNS() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("🌐 WiFi connected! IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("📶 Signal strength: ");
    Serial.println(getConnectionQuality());
    
    if (!MDNS.begin(hostname)) {
      Serial.println("❌ mDNS setup failed!");
    } else {
      Serial.print("🔗 mDNS: http://");
      Serial.print(hostname);
      Serial.println(".local");
      MDNS.addService("http", "tcp", 80);
    }
  } else {
    Serial.println("❌ WiFi failed! GSM-only mode.");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getForestFireHTML());
  });
  
  server.on("/settings", HTTP_GET, []() {
    server.send(200, "text/html", getSettingsHTML());
  });
  
  server.on("/api/data", HTTP_GET, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", getSensorJSON());
  });
  
  server.on("/api/settings", HTTP_POST, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    
    bool settingsChanged = false;
    String responseMessage = "";
    
    // Handle threshold update
    if (server.hasArg("threshold")) {
      int newThreshold = server.arg("threshold").toInt();
      if (newThreshold >= 500 && newThreshold <= 3000) {
        AIR_QUALITY_THRESHOLD = newThreshold;
        settingsChanged = true;
        responseMessage += "Threshold updated to " + String(newThreshold) + ". ";
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid threshold range (500-3000)\"}");
        return;
      }
    }
    
    // Handle temperature offset update
    if (server.hasArg("tempOffset")) {
      float newTempOffset = server.arg("tempOffset").toFloat();
      if (newTempOffset >= -10.0 && newTempOffset <= 10.0) {
        tempOffset = newTempOffset;
        settingsChanged = true;
        responseMessage += "Temperature offset updated to " + String(newTempOffset, 2) + "°C. ";
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid temperature offset (-10 to +10°C)\"}");
        return;
      }
    }
    
    // Handle humidity offset update
    if (server.hasArg("humidityOffset")) {
      float newHumidityOffset = server.arg("humidityOffset").toFloat();
      if (newHumidityOffset >= -20.0 && newHumidityOffset <= 20.0) {
        humidityOffset = newHumidityOffset;
        settingsChanged = true;
        responseMessage += "Humidity offset updated to " + String(newHumidityOffset, 2) + "%. ";
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid humidity offset (-20 to +20%)\"}");
        return;
      }
    }
    
    // FIXED: Enhanced phone number validation and handling
    if (server.hasArg("phoneNumber")) {
      String newPhoneNumber = server.arg("phoneNumber");
      if (newPhoneNumber.length() >= 10 && 
          newPhoneNumber.length() <= 19 &&
          newPhoneNumber.startsWith("+") &&
          newPhoneNumber.charAt(1) != ' ' &&
          newPhoneNumber.indexOf(' ') == -1) {
        phoneNumber = newPhoneNumber;
        settingsChanged = true;
        responseMessage += "Phone number updated to " + newPhoneNumber + ". ";
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid phone number format (must start with + and be 10-19 digits)\"}");
        return;
      }
    }
    
    if (settingsChanged) {
      saveSettings();
      server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"" + responseMessage + "\"}");
      Serial.println("⚙️ Settings updated via web interface");
    } else {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No valid parameters provided\"}");
    }
  });
  
  server.on("/api/alert", HTTP_POST, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (!smsInProgress) {
      sendManualAlertSMSWithProtection();
      server.send(200, "application/json", "{\"status\":\"success\"}");
    } else {
      server.send(503, "application/json", "{\"status\":\"busy\",\"message\":\"SMS in progress\"}");
    }
  });
  
  // NEW: Skip warming endpoint
  server.on("/api/skipwarming", HTTP_POST, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    
    if (!smsInProgress) {
      skipWarmingRequested = true;
      sensorStartTime = millis() - preheatTime; // Force warming to complete
      
      Serial.println("🔥 *** SENSOR WARMING SKIPPED BY USER ***");
      Serial.println("⚡ System now ready for immediate fire detection");
      
      server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Sensor warming skipped - system ready\"}");
    } else {
      server.send(503, "application/json", "{\"status\":\"busy\",\"message\":\"Cannot skip during SMS operation\"}");
    }
  });
  
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  
  server.begin();
  Serial.println("🖥️ Web server started");
}

void sendManualAlertSMSWithProtection() {
  String alertMessage = "🚨 MANUAL FIRE ALERT!\nOperator Triggered\nRisk: " + String(airQualityPercent) + 
                       "%\nT:" + String(temperature, 0) + "C H:" + 
                       String(humidity, 0) + "%\nVoltage: " + String(airQualityVoltage, 2) + "V\nCheck conditions immediately!";
  
  if (sendSMSWithFullProtection(alertMessage, "MANUAL")) {
    totalSMSSent++;
    updateSMSStatus("Manual Alert Sent", "Web Dashboard");
    Serial.println("✅ Protected manual alert sent successfully");
  } else {
    updateSMSStatus("Manual Alert Failed", "SMS Error");
    Serial.println("❌ Protected manual alert failed");
  }
}

bool checkGSMPowerStatus() {
  if (!gsmPowerState) {
    gsmSerial.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    gsmPowerState = true;
    Serial.println("📡 GSM powered up");
    delay(1000);
  }
  
  if (millis() - gsmCooldownTime < GSM_COOLDOWN_PERIOD) {
    Serial.println("📡 GSM cooldown active");
    return false;
  }
  
  return true;
}

void initializeGSMWithPowerManagement() {
  Serial.println("📡 Initializing Forest Fire Detection GSM...");
  
  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  gsmPowerState = true;
  
  delay(2000);
  sendATCommandWithResponse("AT", "OK", 1000);
  delay(1000);
  sendATCommandWithResponse("AT+CMGF=1", "OK", 1000);
  delay(1000);
  sendATCommandWithResponse("AT+CSCS=\"GSM\"", "OK", 1000);
  delay(1000);
  sendATCommandWithResponse("AT+CPIN?", "OK", 2000);
  delay(1000);
  sendATCommandWithResponse("AT+CREG?", "OK", 2000);
  delay(1000);
  sendATCommandWithResponse("AT+CSQ", "OK", 2000);
  
  clearAllSMSMemory();
  Serial.println("📡 Forest Fire Detection GSM ready");
}

void readSensors() {
  float rawTemp = dht.readTemperature();
  float rawHumidity = dht.readHumidity();
  
  temperature = rawTemp + tempOffset;
  humidity = rawHumidity + humidityOffset;
  
  airQualityRaw = analogRead(MQ135_PIN);
  airQualityVoltage = (airQualityRaw * 3.3) / 4095.0;
  
  airQualityPercent = map(airQualityRaw, NORMAL_BASELINE, MAX_EXPECTED, 0, 100);
  airQualityPercent = constrain(airQualityPercent, 0, 100);
  
  if (airQualityPercent <= 25) {
    airQualityStatus = "SAFE";
  } else if (airQualityPercent <= 50) {
    airQualityStatus = "ELEVATED";
  } else if (airQualityPercent <= 75) {
    airQualityStatus = "HIGH RISK";
  } else {
    airQualityStatus = "FIRE ALERT";
  }
  
  if (isnan(rawHumidity) || isnan(rawTemp)) {
    temperature = -999;
    humidity = -999;
  }
}

void updateSMSStatus(String status, String reason) {
  smsStatus = status;
  alertReason = reason;
  
  unsigned long currentTime = millis();
  int hours = (currentTime / 3600000) % 24;
  int minutes = (currentTime / 60000) % 60;
  int seconds = (currentTime / 1000) % 60;
  
  lastSMSTime = String(hours) + ":" + String(minutes) + ":" + String(seconds);
}

void displaySensorData() {
  bool isPreheating = (millis() - sensorStartTime) < preheatTime;
  
  unsigned long currentTime = millis();
  int hours = (currentTime / 3600000) % 24;
  int minutes = (currentTime / 60000) % 60;
  int seconds = (currentTime / 1000) % 60;
  int milliseconds = currentTime % 1000;
  
  Serial.printf("🕐 %02d:%02d:%02d.%03d -> Sensor Value: %d | Voltage: %.2fV\n", 
                hours, minutes, seconds, milliseconds, airQualityRaw, airQualityVoltage);
  
  Serial.println("🔥 --- Forest Fire Detection & Early Warning System Readings ---");
  
  if (temperature != -999) {
    Serial.print("🌡️ Temperature: ");
    Serial.print(temperature);
    Serial.println("°C");
    
    Serial.print("💧 Humidity: ");
    Serial.print(humidity);
    Serial.println("%");
  } else {
    Serial.println("❌ DHT22: ERROR");
  }
  
  Serial.print("🌬️ Air Quality (Raw ADC): ");
  Serial.println(airQualityRaw);
  
  Serial.print("⚡ Air Quality (Voltage): ");
  Serial.print(airQualityVoltage, 2);
  Serial.println("V");
  
  Serial.print("🔥 Fire Risk Level: ");
  if (isPreheating) {
    Serial.println("SENSOR WARMING...");
  } else {
    Serial.print(airQualityPercent);
    Serial.print("% - ");
    Serial.println(airQualityStatus);
  }
  
  Serial.print("📱 SMS Status: ");
  Serial.print(smsStatus);
  if (smsInProgress) {
    Serial.print(" [PROTECTED]");
  }
  Serial.print(" | Total Sent: ");
  Serial.print(totalSMSSent);
  Serial.print(" | Last: ");
  Serial.println(lastSMSTime);
  Serial.print("📞 Phone Number: ");
  Serial.println(phoneNumber);
  
  Serial.print("💾 Free Memory: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.print("⚙️ Current Threshold: ");
  Serial.println(AIR_QUALITY_THRESHOLD);
  Serial.println("🔥 --------------------------------------------------");
}

void calibrateSensors() {
  Serial.println("🔥 === Forest Fire Detection Sensor Calibration ===");
  
  float currentTemp = dht.readTemperature();
  Serial.print("🌡️ Current temp: ");
  Serial.print(currentTemp);
  Serial.println("°C");
  
  Serial.print("🌬️ Current air quality (raw): ");
  Serial.println(airQualityRaw);
  Serial.print("⚡ Current voltage: ");
  Serial.print(airQualityVoltage, 2);
  Serial.println("V");
  Serial.print("⚙️ Current threshold: ");
  Serial.println(AIR_QUALITY_THRESHOLD);
  Serial.print("📞 Phone number: ");
  Serial.println(phoneNumber);
  Serial.print("🔒 SMS Protection: ");
  Serial.println(smsInProgress ? "ACTIVE" : "READY");
  Serial.println("🔥 =======================================");
}

void testGSMConnection() {
  Serial.println("🔧 === Forest Fire Detection GSM Test ===");
  sendATCommandWithResponse("AT", "OK", 1000);
  delay(500);
  sendATCommandWithResponse("AT+CSQ", "OK", 1000);
  delay(500);
  sendATCommandWithResponse("AT+CREG?", "OK", 1000);
  delay(500);
  sendATCommandWithResponse("AT+CPIN?", "OK", 1000);
  Serial.println("🔧 === Test Complete ===");
}

String getSensorJSON() {
  bool isPreheating = (millis() - sensorStartTime) < preheatTime;
  int warmupTimeLeft = isPreheating ? (preheatTime - (millis() - sensorStartTime)) / 1000 : 0;
  
  String json = "{";
  json += "\"temperature\":" + String(temperature, 1) + ",";
  json += "\"humidity\":" + String(humidity, 1) + ",";
  json += "\"tempOffset\":" + String(tempOffset, 2) + ",";
  json += "\"humidityOffset\":" + String(humidityOffset, 2) + ",";
  json += "\"phoneNumber\":\"" + phoneNumber + "\",";
  json += "\"airQualityRaw\":" + String(airQualityRaw) + ",";
  json += "\"airQualityVoltage\":" + String(airQualityVoltage, 2) + ",";
  json += "\"airQualityPercent\":" + String(airQualityPercent) + ",";
  json += "\"airQualityStatus\":\"" + airQualityStatus + "\",";
  json += "\"threshold\":" + String(AIR_QUALITY_THRESHOLD) + ",";
  json += "\"smsStatus\":\"" + smsStatus + "\",";
  json += "\"lastSMSTime\":\"" + lastSMSTime + "\",";
  json += "\"totalSMSSent\":" + String(totalSMSSent) + ",";
  json += "\"alertReason\":\"" + alertReason + "\",";
  json += "\"alertSent\":" + String(alertSent ? "true" : "false") + ",";
  json += "\"isWarming\":" + String(isPreheating ? "true" : "false") + ",";
  json += "\"warmupTimeLeft\":" + String(warmupTimeLeft) + ",";
  json += "\"canSkipWarming\":" + String((isPreheating && !smsInProgress) ? "true" : "false") + ",";
  json += "\"smsProtected\":" + String(smsInProgress ? "true" : "false") + ",";
  json += "\"timestamp\":" + String(millis()) + ",";
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ipAddress\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"signalQuality\":\"" + getConnectionQuality() + "\"";
  json += "}";
  return json;
}

String getSettingsHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>🔥 Forest Fire Detection and Early Warning System Settings</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', sans-serif;
            background: linear-gradient(135deg, #2d5016 0%, #8B4513 50%, #FF4500 100%);
            min-height: 100vh;
            padding: 20px;
            color: white;
        }
        .container { max-width: 800px; margin: 0 auto; }
        .header {
            text-align: center;
            margin-bottom: 30px;
        }
        .header h1 {
            font-size: 2.5rem;
            margin-bottom: 10px;
            text-shadow: 0 2px 4px rgba(0,0,0,0.3);
        }
        .settings-card {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 30px;
            backdrop-filter: blur(10px);
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2);
            color: #2d3436;
            margin-bottom: 20px;
        }
        .settings-section {
            margin-bottom: 30px;
        }
        .settings-section h3 {
            color: #2d3436;
            margin-bottom: 15px;
            border-bottom: 2px solid #ddd;
            padding-bottom: 5px;
        }
        .form-row {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 20px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        .form-group label {
            display: block;
            margin-bottom: 8px;
            font-weight: 600;
            color: #2d3436;
        }
        .form-group input {
            width: 100%;
            padding: 12px;
            border: 2px solid #ddd;
            border-radius: 8px;
            font-size: 1rem;
        }
        .form-group input:focus {
            outline: none;
            border-color: #FF4500;
        }
        .form-group small {
            color: #666;
            font-size: 0.9rem;
            margin-top: 5px;
            display: block;
        }
        .btn {
            background: linear-gradient(45deg, #FF4500, #FF6B35);
            color: white;
            border: none;
            padding: 12px 25px;
            border-radius: 8px;
            font-size: 1rem;
            cursor: pointer;
            margin-right: 10px;
            margin-bottom: 10px;
            transition: transform 0.2s;
        }
        .btn:hover {
            transform: translateY(-2px);
        }
        .btn-secondary {
            background: linear-gradient(45deg, #636e72, #2d3436);
        }
        .btn:disabled {
            opacity: 0.6;
            cursor: not-allowed;
            transform: none;
        }
        .info-section {
            background: rgba(255, 255, 255, 0.1);
            padding: 20px;
            border-radius: 12px;
            margin-top: 20px;
        }
        .calibration-info {
            background: rgba(74, 144, 226, 0.1);
            border-left: 4px solid #4a90e2;
            padding: 15px;
            margin: 15px 0;
            border-radius: 4px;
        }
        .success-message {
            background: rgba(40, 167, 69, 0.1);
            border: 1px solid #28a745;
            color: #28a745;
            padding: 10px;
            border-radius: 4px;
            margin-bottom: 15px;
            display: none;
        }
        .error-message {
            background: rgba(220, 53, 69, 0.1);
            border: 1px solid #dc3545;
            color: #dc3545;
            padding: 10px;
            border-radius: 4px;
            margin-bottom: 15px;
            display: none;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🔥 Forest Fire Detection and Early Warning System Settings</h1>
            <p>Configure alert thresholds, sensor calibration, and notification settings</p>
        </div>
        
        <div class="settings-card">
            <div id="successMessage" class="success-message"></div>
            <div id="errorMessage" class="error-message"></div>
            
            <form id="settingsForm">
                <!-- Fire Detection Settings -->
                <div class="settings-section">
                    <h3>🎛️ Fire Detection Configuration</h3>
                    <div class="form-group">
                        <label for="threshold">Fire Detection Threshold (ADC Value):</label>
                        <input type="number" id="threshold" name="threshold" min="500" max="3000" 
                               value="1200" placeholder="Enter threshold value">
                        <small>Range: 500-3000 | Current: <span id="currentThreshold">Loading...</span></small>
                    </div>
                </div>
                
                <!-- SMS Notification Settings -->
                <div class="settings-section">
                    <h3>📱 SMS Notification Settings</h3>
                    <div class="form-group">
                        <label for="phoneNumber">Recipient Phone Number:</label>
                        <input type="tel" id="phoneNumber" name="phoneNumber" 
                               placeholder="+977xxxxxxxxxx" value=""
                               autocomplete="off" 
                               autocapitalize="off"
                               autocorrect="off"
                               spellcheck="false"
                               data-form-type="other">
                        <small>Format: +[country code][number] (e.g., +9779861234567) | Current: <span id="currentPhoneNumber">Loading...</span></small>
                    </div>
                </div>
                
                <!-- Sensor Calibration Settings -->
                <div class="settings-section">
                    <h3>🔧 Sensor Calibration</h3>
                    <div class="calibration-info">
                        <strong>📋 Calibration Instructions:</strong><br>
                        • Compare your sensor readings with a reference thermometer/hygrometer<br>
                        • If your sensor reads high, use negative values<br>
                        • If your sensor reads low, use positive values<br>
                        • Example: Sensor shows 25°C, reference shows 24°C → use -1.0°C offset
                    </div>
                    
                    <div class="form-row">
                        <div class="form-group">
                            <label for="tempOffset">Temperature Correction (°C):</label>
                            <input type="number" id="tempOffset" name="tempOffset" 
                                   min="-10" max="10" step="0.1" value="0.0" 
                                   placeholder="0.0">
                            <small>Range: -10.0 to +10.0°C | Current: <span id="currentTempOffset">Loading...</span>°C</small>
                        </div>
                        
                        <div class="form-group">
                            <label for="humidityOffset">Humidity Correction (%):</label>
                            <input type="number" id="humidityOffset" name="humidityOffset" 
                                   min="-20" max="20" step="0.1" value="0.0" 
                                   placeholder="0.0">
                            <small>Range: -20.0 to +20.0% | Current: <span id="currentHumidityOffset">Loading...</span>%</small>
                        </div>
                    </div>
                </div>
                
                <button type="submit" class="btn">💾 Save All Settings</button>
                <button type="button" class="btn btn-secondary" onclick="resetDefaults()">🔄 Reset Defaults</button>
                <button type="button" class="btn btn-secondary" onclick="goBack()">🏠 Back to Dashboard</button>
            </form>
        </div>
        
        <div class="info-section">
            <h3>📊 Current System Status</h3>
            <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin-top: 15px;">
                <div>
                    <strong>IP Address:</strong><br>
                    <span id="ipAddress">Loading...</span>
                </div>
                <div>
                    <strong>WiFi Signal:</strong><br>
                    <span id="wifiSignal">Loading...</span>
                </div>
                <div>
                    <strong>Current Reading:</strong><br>
                    <span id="currentReading">Loading...</span>
                </div>
                <div>
                    <strong>Corrected Values:</strong><br>
                    <span id="correctedValues">Loading...</span>
                </div>
            </div>
        </div>
    </div>

    <script>
        let currentData = {};
        
        function loadSystemInfo() {
            fetch('/api/data')
                .then(response => response.json())
                .then(data => {
                    currentData = data;
                    
                    document.getElementById('currentThreshold').textContent = data.threshold;
                    document.getElementById('threshold').value = data.threshold;
                    
                    document.getElementById('currentPhoneNumber').textContent = data.phoneNumber || 'Not set';
                    document.getElementById('phoneNumber').value = data.phoneNumber || '';
                    
                    document.getElementById('currentTempOffset').textContent = (data.tempOffset || 0).toFixed(1);
                    document.getElementById('currentHumidityOffset').textContent = (data.humidityOffset || 0).toFixed(1);
                    document.getElementById('tempOffset').value = data.tempOffset || 0;
                    document.getElementById('humidityOffset').value = data.humidityOffset || 0;
                    
                    document.getElementById('ipAddress').textContent = data.ipAddress;
                    document.getElementById('wifiSignal').textContent = data.signalQuality;
                    document.getElementById('currentReading').textContent = data.airQualityRaw + ' ADC (' + data.airQualityVoltage + 'V)';
                    document.getElementById('correctedValues').textContent = 
                        data.temperature.toFixed(1) + '°C, ' + data.humidity.toFixed(1) + '%';
                })
                .catch(error => {
                    console.error('Error loading system info:', error);
                    showError('Failed to load system information');
                });
        }
        
        // Enhanced phone number field handling
        document.getElementById('phoneNumber').addEventListener('focus', function() {
            // Clear browser suggestions and select existing text
            this.setAttribute('autocomplete', 'new-password');
            if (this.value && this.value === currentData.phoneNumber) {
                this.select();
            }
        });

        document.getElementById('phoneNumber').addEventListener('blur', function() {
            // Validate format on blur
            const phonePattern = /^\+[1-9]\d{9,18}$/;
            if (this.value && !phonePattern.test(this.value)) {
                showError('Invalid phone number format. Use +[country][number]');
                this.focus();
            }
        });

        // Prevent browser form caching
        window.addEventListener('load', function() {
            document.getElementById('phoneNumber').setAttribute('autocomplete', 'off');
        });
        
        function showSuccess(message) {
            const successDiv = document.getElementById('successMessage');
            successDiv.textContent = message;
            successDiv.style.display = 'block';
            document.getElementById('errorMessage').style.display = 'none';
            setTimeout(() => {
                successDiv.style.display = 'none';
            }, 5000);
        }
        
        function showError(message) {
            const errorDiv = document.getElementById('errorMessage');
            errorDiv.textContent = message;
            errorDiv.style.display = 'block';
            document.getElementById('successMessage').style.display = 'none';
            setTimeout(() => {
                errorDiv.style.display = 'none';
            }, 5000);
        }
        
        document.getElementById('settingsForm').addEventListener('submit', function(e) {
            e.preventDefault();
            
            const formData = new FormData();
            formData.append('threshold', document.getElementById('threshold').value);
            formData.append('phoneNumber', document.getElementById('phoneNumber').value);
            formData.append('tempOffset', document.getElementById('tempOffset').value);
            formData.append('humidityOffset', document.getElementById('humidityOffset').value);
            
            const btn = e.target.querySelector('button[type="submit"]');
            const originalText = btn.innerHTML;
            
            btn.innerHTML = '💾 Saving...';
            btn.disabled = true;
            
            const params = new URLSearchParams();
            for (let [key, value] of formData) {
                params.append(key, value);
            }
            
            fetch('/api/settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: params.toString()
            })
            .then(response => response.json())
            .then(data => {
                if (data.status === 'success') {
                    btn.innerHTML = '✅ Saved!';
                    showSuccess(data.message || 'Settings saved successfully!');
                    setTimeout(() => {
                        btn.innerHTML = originalText;
                        btn.disabled = false;
                        loadSystemInfo();
                    }, 2000);
                } else {
                    btn.innerHTML = '❌ Error';
                    showError(data.message || 'Failed to save settings');
                    setTimeout(() => {
                        btn.innerHTML = originalText;
                        btn.disabled = false;
                    }, 2000);
                }
            })
            .catch(error => {
                console.error('Error saving settings:', error);
                btn.innerHTML = '❌ Error';
                showError('Network error occurred');
                setTimeout(() => {
                    btn.innerHTML = originalText;
                    btn.disabled = false;
                }, 2000);
            });
        });
        
        function resetDefaults() {
            document.getElementById('threshold').value = 1200;
            document.getElementById('phoneNumber').value = '+9779863797105';
            document.getElementById('tempOffset').value = 0.0;
            document.getElementById('humidityOffset').value = 0.0;
        }
        
        function goBack() {
            window.location.href = '/';
        }
        
        loadSystemInfo();
        setInterval(loadSystemInfo, 10000);
    </script>
</body>
</html>
)rawliteral";
  
  return html;
}

String getForestFireHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>🔥 Forest Fire Detection and Early Warning System</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', sans-serif;
            background: linear-gradient(135deg, #2d5016 0%, #8B4513 50%, #FF4500 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 1000px; margin: 0 auto; }
        .header {
            text-align: center;
            margin-bottom: 30px;
            color: white;
        }
        .header h1 {
            font-size: 2.5rem;
            font-weight: 300;
            margin-bottom: 8px;
            text-shadow: 0 2px 4px rgba(0,0,0,0.3);
        }
        .header p {
            font-size: 1.1rem;
            opacity: 0.9;
        }
        .network-info {
            background: rgba(255, 255, 255, 0.15);
            backdrop-filter: blur(10px);
            border-radius: 12px;
            padding: 12px 20px;
            margin-bottom: 20px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            color: white;
            font-size: 0.9rem;
        }
        .sms-notification {
            background: rgba(255, 255, 255, 0.15);
            backdrop-filter: blur(10px);
            border-radius: 15px;
            padding: 12px 20px;
            margin-bottom: 25px;
            display: flex;
            align-items: center;
            justify-content: space-between;
            color: white;
            font-size: 0.9rem;
        }
        .sms-info {
            display: flex;
            align-items: center;
            gap: 15px;
        }
        .sms-icon-small {
            width: 20px;
            height: 20px;
            border-radius: 50%;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 12px;
        }
        .sms-ready-small { background: #228B22; }
        .sms-cooldown-small { background: #FFD700; }
        .sms-sending-small { background: #FF6B35; }
        .sms-error-small { background: #DC143C; }
        .sms-protected-small { 
            background: #74b9ff; 
            animation: protectedPulse 1.5s infinite;
        }
        @keyframes protectedPulse {
            0% { opacity: 0.6; transform: scale(0.9); }
            50% { opacity: 1; transform: scale(1.1); }
            100% { opacity: 0.6; transform: scale(0.9); }
        }
        .dashboard-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
            gap: 20px;
            margin-bottom: 25px;
        }
        .sensor-card {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 25px;
            backdrop-filter: blur(10px);
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2);
            transition: transform 0.3s ease;
        }
        .sensor-card:hover { transform: translateY(-3px); }
        .sensor-icon {
            width: 50px;
            height: 50px;
            border-radius: 12px;
            display: flex;
            align-items: center;
            justify-content: center;
            margin-bottom: 15px;
            font-size: 20px;
        }
        .climate-icon { background: linear-gradient(45deg, #228B22, #32CD32); }
        .fire-icon { background: linear-gradient(45deg, #FF4500, #DC143C); }
        .sensor-label {
            font-size: 0.85rem;
            color: #666;
            text-transform: uppercase;
            letter-spacing: 1px;
            margin-bottom: 8px;
            font-weight: 500;
        }
        .climate-values {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 10px;
        }
        .temp-humidity {
            display: flex;
            align-items: baseline;
            gap: 15px;
        }
        .temp-value, .humidity-value {
            font-size: 2.2rem;
            font-weight: 200;
            color: #2d3436;
        }
        .temp-unit, .humidity-unit {
            font-size: 1rem;
            color: #636e72;
            margin-left: 2px;
        }
        .divider {
            font-size: 2rem;
            color: #ddd;
            margin: 0 5px;
        }
        .sensor-value {
            font-size: 2.5rem;
            font-weight: 200;
            color: #2d3436;
            margin-bottom: 10px;
            line-height: 1;
        }
        .sensor-unit {
            font-size: 1.1rem;
            color: #636e72;
            font-weight: 400;
        }
        .voltage-display {
            font-size: 0.75rem;
            color: #888;
            margin-top: 5px;
            font-style: italic;
        }
        .warming-indicator {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-top: 8px;
            font-size: 0.75rem;
            color: #FF6B35;
        }
        .warming-bulb {
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background: #FF6B35;
            margin-right: 6px;
            animation: pulse 1.5s infinite;
        }
        .warming-text {
            flex: 1;
        }
        .skip-warming-btn {
            background: linear-gradient(45deg, #FF6B35, #FFD700);
            color: white;
            border: none;
            padding: 4px 8px;
            border-radius: 4px;
            font-size: 0.7rem;
            cursor: pointer;
            margin-left: 8px;
            transition: all 0.2s ease;
        }
        .skip-warming-btn:hover {
            transform: scale(1.05);
            background: linear-gradient(45deg, #FF4500, #FF6B35);
        }
        .skip-warming-btn:disabled {
            opacity: 0.5;
            cursor: not-allowed;
            transform: none;
        }
        @keyframes pulse {
            0% { opacity: 0.4; transform: scale(0.8); }
            50% { opacity: 1; transform: scale(1.1); }
            100% { opacity: 0.4; transform: scale(0.8); }
        }
        .status-badge {
            display: inline-block;
            padding: 6px 12px;
            border-radius: 15px;
            font-size: 0.8rem;
            font-weight: 500;
            text-transform: uppercase;
            margin-top: 8px;
        }
        .status-safe { background: linear-gradient(45deg, #228B22, #32CD32); color: white; }
        .status-elevated { background: linear-gradient(45deg, #FFD700, #FF6B35); color: white; }
        .status-high { background: linear-gradient(45deg, #FF6B35, #DC143C); color: white; }
        .status-fire { background: linear-gradient(45deg, #DC143C, #8B0000); color: white; }
        .status-warming { background: linear-gradient(45deg, #FF6B35, #FFD700); color: white; }
        .controls {
            display: flex;
            justify-content: center;
            gap: 15px;
            margin-bottom: 25px;
            flex-wrap: wrap;
        }
        .btn {
            background: rgba(255, 255, 255, 0.9);
            border: none;
            padding: 12px 25px;
            border-radius: 12px;
            font-size: 0.95rem;
            font-weight: 500;
            cursor: pointer;
            transition: all 0.3s ease;
            color: #2d3436;
        }
        .btn:hover {
            transform: translateY(-2px);
            background: rgba(255, 255, 255, 1);
        }
        .btn-alert {
            background: linear-gradient(45deg, #DC143C, #FF4500);
            color: white;
        }
        .btn-settings {
            background: linear-gradient(45deg, #636e72, #2d3436);
            color: white;
        }
        .btn:disabled {
            opacity: 0.5;
            cursor: not-allowed;
        }
        .last-update {
            background: rgba(255, 255, 255, 0.1);
            padding: 12px;
            border-radius: 12px;
            text-align: center;
            color: white;
            backdrop-filter: blur(10px);
            font-size: 0.9rem;
        }
        @media (max-width: 768px) {
            .dashboard-grid { grid-template-columns: 1fr; }
            .controls { flex-direction: column; align-items: center; }
            .sms-notification { flex-direction: column; gap: 8px; text-align: center; }
            .network-info { flex-direction: column; gap: 8px; text-align: center; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🔥 Forest Fire Detection and Early Warning System</h1>
            <p>Real-time monitoring with fixed SMS alerts and comprehensive settings</p>
        </div>
        
        <div class="network-info">
            <div>📱 Mobile Access: <strong id="ipaddress">Loading...</strong></div>
            <div>📶 WiFi: <strong id="wifisignal">Loading...</strong></div>
            <div>⚙️ Threshold: <strong id="threshold">Loading...</strong></div>
        </div>
        
        <div class="sms-notification">
            <div class="sms-info">
                <div class="sms-icon-small" id="smsicon">🚨</div>
                <span>Alert System: <strong id="smsstatus">Ready</strong></span>
                <span>Sent: <strong id="totalsms">0</strong></span>
                <span>Last: <strong id="lastsms">Never</strong></span>
                <span>To: <strong id="phonenumber">Loading...</strong></span>
                <span id="protectionStatus" style="display:none;">🔒 PROTECTED</span>
            </div>
            <div id="alertreason" style="font-size: 0.8rem; opacity: 0.8;">Normal</div>
        </div>
        
        <div class="dashboard-grid">
            <div class="sensor-card">
                <div class="sensor-icon climate-icon">🌡️</div>
                <div class="sensor-label">Environmental Conditions</div>
                <div class="climate-values">
                    <div class="temp-humidity">
                        <div>
                            <span class="temp-value" id="temperature">--</span>
                            <span class="temp-unit">°C</span>
                        </div>
                        <div class="divider">|</div>
                        <div>
                            <span class="humidity-value" id="humidity">--</span>
                            <span class="humidity-unit">%</span>
                        </div>
                    </div>
                </div>
            </div>
            
            <div class="sensor-card">
                <div class="sensor-icon fire-icon">🔥</div>
                <div class="sensor-label">Fire Risk Assessment</div>
                <div class="sensor-value" id="airquality">--</div>
                <div class="sensor-unit">% Risk Level</div>
                <div class="voltage-display" id="voltage">Sensor: --V</div>
                <div class="warming-indicator" id="warmingindicator" style="display: none;">
                    <div style="display: flex; align-items: center; flex: 1;">
                        <div class="warming-bulb"></div>
                        <div class="warming-text" id="warmingtext">Sensor warming up...</div>
                    </div>
                    <button class="skip-warming-btn" id="skipWarmingBtn" onclick="skipWarming()">⚡ Skip</button>
                </div>
                <div class="status-badge" id="airstatus">Loading...</div>
            </div>
        </div>
        
        <div class="controls">
            <button class="btn" onclick="refreshData()">🔄 Refresh</button>
            <button class="btn btn-alert" id="alertBtn" onclick="sendAlert()">🚨 Fire Alert</button>
            <button class="btn btn-settings" onclick="openSettings()">⚙️ Settings</button>
        </div>
        
        <div class="last-update">
            <strong>Last Updated:</strong> <span id="lastupdate">--</span>
        </div>
    </div>

    <script>
        function refreshData() {
            fetch('/api/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('ipaddress').textContent = data.ipAddress;
                    document.getElementById('wifisignal').textContent = data.signalQuality;
                    document.getElementById('threshold').textContent = data.threshold;
                    document.getElementById('phonenumber').textContent = data.phoneNumber || 'Not set';
                    
                    document.getElementById('temperature').textContent = data.temperature.toFixed(1);
                    document.getElementById('humidity').textContent = data.humidity.toFixed(1);
                    
                    document.getElementById('airquality').textContent = data.airQualityPercent;
                    document.getElementById('voltage').textContent = 'Sensor: ' + data.airQualityVoltage + 'V';
                    
                    const warmingIndicator = document.getElementById('warmingindicator');
                    const warmingText = document.getElementById('warmingtext');
                    const skipBtn = document.getElementById('skipWarmingBtn');
                    
                    if (data.isWarming) {
                        warmingIndicator.style.display = 'flex';
                        warmingText.textContent = 'Sensor warming up (' + data.warmupTimeLeft + 's left)';
                        skipBtn.disabled = !data.canSkipWarming;
                        skipBtn.style.display = 'block';
                    } else {
                        warmingIndicator.style.display = 'none';
                    }
                    
                    const statusElement = document.getElementById('airstatus');
                    if (data.isWarming) {
                        statusElement.textContent = 'SENSOR WARMING';
                        statusElement.className = 'status-badge status-warming';
                    } else {
                        statusElement.textContent = data.airQualityStatus;
                        statusElement.className = 'status-badge';
                        
                        switch(data.airQualityStatus) {
                            case 'SAFE': statusElement.classList.add('status-safe'); break;
                            case 'ELEVATED': statusElement.classList.add('status-elevated'); break;
                            case 'HIGH RISK': statusElement.classList.add('status-high'); break;
                            case 'FIRE ALERT': statusElement.classList.add('status-fire'); break;
                        }
                    }
                    
                    document.getElementById('smsstatus').textContent = data.smsStatus;
                    document.getElementById('totalsms').textContent = data.totalSMSSent;
                    document.getElementById('lastsms').textContent = data.lastSMSTime;
                    document.getElementById('alertreason').textContent = data.alertReason;
                    
                    const smsIcon = document.getElementById('smsicon');
                    const protectionStatus = document.getElementById('protectionStatus');
                    const alertBtn = document.getElementById('alertBtn');
                    
                    smsIcon.className = 'sms-icon-small';
                    
                    if (data.smsProtected) {
                        smsIcon.classList.add('sms-protected-small');
                        protectionStatus.style.display = 'inline';
                        alertBtn.disabled = true;
                        alertBtn.textContent = '🔒 SMS Protected';
                    } else {
                        protectionStatus.style.display = 'none';
                        alertBtn.disabled = false;
                        alertBtn.innerHTML = '🚨 Fire Alert';
                        
                        if (data.smsStatus.includes('Ready')) {
                            smsIcon.classList.add('sms-ready-small');
                        } else if (data.smsStatus.includes('Cooldown') || data.smsStatus.includes('Preheating')) {
                            smsIcon.classList.add('sms-cooldown-small');
                        } else if (data.smsStatus.includes('Sending') || data.smsStatus.includes('Transmitting')) {
                            smsIcon.classList.add('sms-sending-small');
                        } else if (data.smsStatus.includes('Error')) {
                            smsIcon.classList.add('sms-error-small');
                        } else {
                            smsIcon.classList.add('sms-ready-small');
                        }
                    }
                    
                    document.getElementById('lastupdate').textContent = new Date().toLocaleTimeString();
                })
                .catch(error => {
                    console.error('Error:', error);
                    document.getElementById('airstatus').textContent = 'Connection Error';
                });
        }
        
        function skipWarming() {
            const skipBtn = document.getElementById('skipWarmingBtn');
            if (skipBtn.disabled) return;
            
            const originalText = skipBtn.innerHTML;
            skipBtn.innerHTML = '⚡ Skipping...';
            skipBtn.disabled = true;
            
            fetch('/api/skipwarming', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    if (data.status === 'success') {
                        skipBtn.innerHTML = '✅ Skipped!';
                        setTimeout(() => {
                            refreshData(); // Refresh to hide warming indicator
                        }, 1000);
                    } else {
                        skipBtn.innerHTML = '❌ Error';
                        setTimeout(() => {
                            skipBtn.innerHTML = originalText;
                            skipBtn.disabled = false;
                        }, 2000);
                    }
                })
                .catch(error => {
                    skipBtn.innerHTML = '❌ Error';
                    setTimeout(() => {
                        skipBtn.innerHTML = originalText;
                        skipBtn.disabled = false;
                    }, 2000);
                });
        }
        
        function sendAlert() {
            const alertBtn = document.getElementById('alertBtn');
            if (alertBtn.disabled) return;
            
            const originalText = alertBtn.innerHTML;
            alertBtn.innerHTML = '🚨 Sending...';
            alertBtn.disabled = true;
            
            fetch('/api/alert', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    if (data.status === 'success') {
                        alertBtn.innerHTML = '✅ Alert Sent!';
                    } else if (data.status === 'busy') {
                        alertBtn.innerHTML = '🔒 SMS Busy';
                    } else {
                        alertBtn.innerHTML = '❌ Error';
                    }
                    
                    setTimeout(() => {
                        if (!alertBtn.disabled) {
                            alertBtn.innerHTML = originalText;
                        }
                    }, 3000);
                })
                .catch(error => {
                    alertBtn.innerHTML = '❌ Error';
                    setTimeout(() => {
                        if (!alertBtn.disabled) {
                            alertBtn.innerHTML = originalText;
                        }
                    }, 3000);
                });
        }
        
        function openSettings() {
            window.location.href = '/settings';
        }
        
        setInterval(refreshData, 5000);
        refreshData();
    </script>
</body>
</html>
)rawliteral";
  
  return html;
}
