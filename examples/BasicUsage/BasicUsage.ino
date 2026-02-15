/**
 * ESP32AI Basic Usage Example
 * 
 * Copyright (c) 2026 Xorlent
 * Licensed under the MIT License.
 * https://github.com/Xorlent/ESP32AI
 * 
 * This example demonstrates basic setup and usage of the ESP32AI library
 * with a simple LED control skill emulating two smart lights (porch and living room)
 * 
 * Hardware Requirements:
 * - ESP32 board
 * - I2S MEMS microphone (e.g., INMP441)
 * - Push button for recording trigger
 * - LED on GPIO 2 (built-in LED on most ESP-WROOM32 dev boards)
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

// LED pin
#define LED_PIN 2

// Create ESP32AI instance
// Use Serial Monitor command 'S' to configure settings on first run
ESP32AI ai;

// Skills JSON definition
const char* skillsJson = R"([
  {
    "Target": "Porch vestibule entry",
    "Options": ["On", "Off"],
    "Actions": [
      {
        "Name": "On",
        "Type": "boolean",
        "Variants": ["turn on", "power on", "switch on", "light on"]
      },
      {
        "Name": "Off",
        "Type": "boolean",
        "Variants": ["turn off", "power off", "switch off", "light off"]
      }
    ]
  },
  {
    "Target": "Living great room",
    "Options": ["On", "Off"],
    "Actions": [
      {
        "Name": "On",
        "Type": "boolean",
        "Variants": ["turn on", "power on", "switch on", "light on"]
      },
      {
        "Name": "Off",
        "Type": "boolean",
        "Variants": ["turn off", "power off", "switch off", "light off"]
      }
    ]
  }
])";

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n=== ESP32AI Basic Example ===");
    
    // Initialize LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
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
    
    // Configure the I2S microphone (tested with INMP441)
    ai.configureI2S(I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_PIN);
    
    // Configure recording button (activate by grounding)
    ai.configureRecordingPin(RECORD_BUTTON_PIN, true);
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
    
    // Checks if recording button is pressed and if so, processes audio command
    if (ai.startListening()) {
        // Button was pressed and recording completed
        // Default processCommand timeout is 12000ms to allow for LLM response
        // To adjust, example: ai.processCommand(15000)
        SkillResponse response = ai.processCommand();
        
        // Handle the response
        if (response.hasAction) {
            Serial.printf("Action received: %s -> %s\n", 
                         response.targetName.c_str(), 
                         response.actionName.c_str());
            
            // Execute the action
            if (response.targetName == "Porch vestibule entry") {
                if (response.actionName == "On") {
                    digitalWrite(LED_PIN, HIGH);
                    Serial.println("Porch light turned ON");
                } else if (response.actionName == "Off") {
                    digitalWrite(LED_PIN, LOW);
                    Serial.println("Porch light turned OFF");
                }
            }
            else if (response.targetName == "Living great room") {
                if (response.actionName == "On") {
                    digitalWrite(LED_PIN, HIGH);
                    Serial.println("Living room light turned ON");
                    delay(1000);
                    digitalWrite(LED_PIN, LOW);
                    delay(1000);
                    digitalWrite(LED_PIN, HIGH);
                } else if (response.actionName == "Off") {
                    digitalWrite(LED_PIN, LOW);
                    Serial.println("Living room light turned OFF");
                    delay(1000);
                    digitalWrite(LED_PIN, HIGH);
                    delay(1000);
                    digitalWrite(LED_PIN, LOW);
                }
            }
        } else {
            Serial.println("No matching action detected.");
        }
    }
    //Yield to OS tasks
    delay(50);
}
