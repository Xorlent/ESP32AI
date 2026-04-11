/**
 * ESP32AI Thermostat Control Example
 * 
 * Copyright (c) 2026 Xorlent
 * Licensed under the MIT License.
 * https://github.com/Xorlent/ESP32AI
 * 
 * This example demonstrates basic setup and usage of the ESP32AI library
 * with multiple actions and value parameters emulating a home thermostat
 * 
 * Hardware Requirements:
 * - ESP32 board
 * - I2S MEMS microphone (e.g., INMP441)
 * - Push button for recording trigger
 * 
 * Connections:
 * - I2S Microphone:
 *   - SCK (BCK) -> GPIO 18
 *   - WS        -> GPIO 21
 *   - SD (Data) -> GPIO 19
 * - Push Button -> GPIO 23 (active LOW with internal pullup)
 * 
 * Setup Instructions:
 * 1. Update WiFi credentials below
 * 2. Upload sketch to ESP32
 * 3. Open Serial Monitor - setup will run automatically on first boot
 * 4. After setup, send 'C' to calibrate silence threshold
 * 5. Press button to speak a voice command (3.5s max)
 *    Examples of valid commands: "Turn heater on", "Set thermostat to 80 degrees", "Turn thermostat fan on", "Turn thermostat off", etc...
 * 
 * Serial Monitor Commands:
 * - 'S' = Configure/update credentials
 * - 'C' = Calibrate microphone silence threshold
 */

#include <WiFi.h>
#include <ESP32AI.h>

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// I2S Microphone pins
#define I2S_BCK_PIN 18
#define I2S_WS_PIN 21
#define I2S_DATA_PIN 19

// Recording trigger button
#define RECORD_BUTTON_PIN 23

// Create ESP32AI instance (loads configuration from NVS)
// Use Serial Monitor command 'S' to configure credentials on first run
ESP32AI ai;

// Thermostat state
struct {
    bool power = false;
    String mode = "Off";  // Off, Fan, Heat, Cool
    int setpoint = 72;    // Temperature setpoint
} thermostat;

// Skills JSON definition
const char* skillsJson = R"([
  {
    "Target": "Thermostat",
    "Options": ["Thermostat","Fan", "Heat", "Cool", "Set"],
    "Actions": [
      {
        "Name": "Thermostat",
        "Type": "switch",
        "Variants": ["thermostat off"]
      },
      {
        "Name": "Fan",
        "Type": "switch",
        "Variants": ["fan on", "fan auto"]
      },
      {
        "Name": "Heat",
        "Type": "boolean",
        "Variants": ["heat on", "heater on"]
      },
      {
        "Name": "Cool",
        "Type": "boolean",
        "Variants": ["cool on", "ac on", "air conditioning on"]
      },
      {
        "Name": "Set",
        "Type": "integer",
        "Variants": ["cool to", "set ac to", "heat to", "set heat to", "set heater to"]
      }
    ]
  }
])";

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n=== ESP32AI Thermostat Control ===");
    
    // Connect to WiFi
    Serial.println("Connecting to WiFi");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(2000);
        Serial.println("Awaiting Wi-Fi...");
    }
    Serial.println("WiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Initialize ESP32AI library.
    // Halt if initialization fails, since it is critical to sketch functionality.
    if (!ai.begin()) {
        while (1) delay(1000);
    }
    
    // Set skills configuration immediately after ESP32AI initialization.
    // Halt if initialization fails, since it is critical to sketch functionality.
    if (!ai.setSkills(skillsJson)) {
        while (1) delay(1000);
    }
    
    // Configure I2S microphone
    ai.configureI2S(I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_PIN);
    
    // Configure recording button
    ai.configureRecordingPin(RECORD_BUTTON_PIN, true);

    printStatus();
}

void loop() {
    // Check for serial commands, process requests for ESP32AI calibration or setup
    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'C' || cmd == 'c') {
            Serial.println("\n>>> Calibration requested via Serial Monitor");
            ai.calibrateSilenceThreshold();
        } else if (cmd == 'S' || cmd == 's') {
            Serial.println("\n>>> Setup requested via Serial Monitor...");
            ai.runSetup();
        }
    }
    
    if (ai.startListening()) {
        // Button was pressed and recording completed
        // Default processCommand timeout is 12000ms to allow for LLM response
        // To adjust, example: ai.processCommand(15000)
        SkillResponse response = ai.processCommand();
        
        // Handle the response
        if (response.hasAction && response.targetName == "Thermostat") {
            handleThermostatAction(response.actionName, response.actionValue);
            printStatus();
        } else if (!response.hasAction) {
            Serial.println("No matching action detected.");
        }
    }
    //Yield to OS tasks
    delay(50);
}

void handleThermostatAction(const String& action, const String& value) {
    Serial.printf("Executing: %s", action.c_str());
    if (value.length() > 0) {
        Serial.printf(" (value: %s)", value.c_str());
    }
    Serial.println();

    if (action == "Thermostat") {
        thermostat.power = false;
    }
    if (action == "Fan") {
        thermostat.power = true;
        thermostat.mode = "Fan";
    }
    else if (action == "Heat") {
        thermostat.power = true;
        thermostat.mode = "Heat";
    }
    else if (action == "Cool") {
        thermostat.power = true;
        thermostat.mode = "Cool";
    }
    else if (action == "Set") {
        if (value.length() > 0) {
            int temp = value.toInt();
            if (temp >= 60 && temp <= 85) {
                thermostat.setpoint = temp;
                thermostat.power = true;
                Serial.printf("Temperature set to %d°F\n", temp);
            } else {
                Serial.println("Temperature out of range (60-85°F)");
            }
        }
    }
}

void printStatus() {
    Serial.println("\n--- Thermostat Status ---");
    Serial.printf("Power: %s\n", thermostat.power ? "ON" : "OFF");
    Serial.printf("Mode: %s\n", thermostat.mode.c_str());
    Serial.printf("Setpoint: %d°F\n", thermostat.setpoint);
    Serial.println("------------------------\n");
}
