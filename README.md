# ESP32AI

**AI-powered voice command library for ESP32 with I2S microphone support**

ESP32AI Voice Command enables ESP32 devices to process voice commands via Cloudflare Workers AI. It transcribes audio recorded from an I2S microphone and parses commands based on predefined skills using keyword matching or artificial intelligence.

---

## Features

- **I2S Audio Recording**: Records mono audio at 8kHz, 8-bit from I2S MEMS microphones
- **Block-Based Audio Compression**: Real-time dynamic range compression with per-chunk gain adjustment
- **Automatic Silence Detection**: Calibrated noise floor detection prevents background noise amplification
- **NVS Credential Storage**: Automatically saves and retrieves DeviceID, Authorization tokens, and calibration data
- **Skill-Based System**: Define custom voice commands with flexible action variants
- **GPIO Trigger**: Start/stop recording with a physical button
- **HTTPS Communication**: Secure communication with your Cloudflare Worker
- **Easy Integration**: Simple non-blocking API for easy loop() integration

---

## How It Works

```
1. You press the record button
   ↓
2. ESP32 records command with dynamic gain control
   ↓
3. ESP32 processes and packages the audio
   ↓
4. Audio and skills are sent to your Cloudflare Worker via HTTPS
   ↓
5. Worker uses AI to:
   - Convert speech to text (Whisper AI)
   - Match text to your defined skills (keyword matching or Mistral AI)
   ↓
6. Worker returns which action to execute
   ↓
7. ESP32 receives the action and your code decides what to do
   (turn on LED, set temperature, etc.)
```

**Key Points:**
- **Your ESP32** handles recording and compression (no cloud dependency for audio processing)
- **Your Cloudflare Worker** runs the AI (free tier includes 10,000 AI requests/day)
- **You define** what voice commands mean via the "skills" JSON
- **You control** what happens when actions are triggered (the response handling)

**Privacy Notes:**
- Audio is only recorded when you press the button
- Audio goes to **your** Cloudflare Worker, but is processed by Whisper AI (@cf/openai/whisper)
- Transcribed audio is then keyword matched by the Cloudflare Worker
- If the Worker is unable to match a skill or finds ambiguity, the transcribed audio and skills JSON is sent to a Mistral AI model (@hf/mistral/mistral-7b-instruct-v0.2) for a decision

---

## Hardware Requirements

- **ESP32** board (ESP32, ESP32-S2, ESP32-S3, etc.)
- **I2S MEMS Microphone** (e.g., INMP441, ICS-43434, SPH0645)
- **Push Button** for recording trigger
- **WiFi Connection** (managed separately in your sketch)

### Recommended Microphone: INMP441

| INMP441 Pin | ESP32 Pin (Example) |   Description   |
|-------------|---------------------|-----------------|
| SCK (BCLK)  | GPIO 18             | Bit Clock       |
| WS (LRCLK)  | GPIO 21             | Word Select     |
| SD (DATA)   | GPIO 19             | Serial Data     |
| L/R         | GND                 | Left channel    |
| VDD         | 3.3V                | Power (NOT 5V!) |
| GND         | GND                 | Ground          |

**Important:** INMP441 uses 3.3V, not 5V! Connecting to 5V can damage the microphone.

**Note on Pins:** You can use any appropriate GPIO pins - just update them in your code with `configureI2S()`. The pins shown above match the BasicUsage example.

### Button Connection (Simple!)

**Most Common (Normally open single pole):**
```
Button ----[one side]---- GPIO 23
       ----[other side]--- GND
```
Then use: `ai.configureRecordingPin(23, true);`

---

## Installation

### Arduino IDE

1. Download this repository as a ZIP file
2. In Arduino IDE: **Sketch → Include Library → Add .ZIP Library**
3. Select the downloaded ZIP file
4. Restart Arduino IDE

## What You'll Need (Complete Beginners)

If you're new to ESP32 and voice control, here's what you need to get started:

### 1. Hardware ($15-30 total)
- **ESP32 Development Board** (~$8) - Any ESP32 board works (ESP32, ESP32-S2, ESP32-S3)
  - Recommended: ESP32-WROOM-32 DevKit
- **I2S MEMS Microphone** (~$3) - For recording voice commands
  - Recommended: INMP441 (most common, well-supported)
  - Alternatives: ICS-43434, SPH0645
- **Push Button** (~$1) - Normally open single pole switch or tact button
- **Jumper Wires** (~$1) - For connections

### 2. Software
- **Arduino IDE** - Download from [arduino.cc](https://www.arduino.cc/en/software)
- **ESP32 Board Support** - ESP32 Board Package: esp32 by Espressif Systems 3.3.5 or later
- **This Library** - Install via the method above or via the Arduino Package Manager
- **Cloudflare Account** (free) - For the AI voice processing backend

## Quick Start

1. **Install the library** (5 min) - Use Arduino Library Manager or ZIP install above
2. **Wire up the microphone** (5 min) - 6 jumper wire connections
3. **Deploy Cloudflare Worker** (10 min) - Copy/paste, no coding needed (see instructions in companion worker.js files)
4. **Upload the example** (5 min) - Connect board, open BasicUsage.ino, update your WiFi details, upload sketch
5. **Configure via Serial Monitor** (3 min) - Enter your Cloudflare Worker URL
6. **Test voice commands!** (2 min) - Press button, say "turn on living room"

### First Run Experience

When you first run the code:
1. **Serial Monitor** will prompt you for:
   - Device ID (e.g., "ESP32-Device-001")
   - Authorization token (e.g., "Bearer your-token-here")
   - Worker endpoint URL (e.g., "https://your-worker.workers.dev")
2. All settings are **saved to NVS** (flash memory)
3. Update settings at any time by sending **'S'** in Serial Monitor

## API Reference

### Constructor

```cpp
ESP32AI(const char* deviceID = nullptr, const char* authorization = nullptr, const char* workerAIEndpoint = nullptr)
```

Creates an ESP32AI instance. If credentials are provided, they are saved to NVS. If omitted, credentials are loaded from NVS (if previously saved).

**Parameters:**
- `deviceID` - Unique device identifier (optional)
- `authorization` - Authorization token, typically "Bearer TOKEN" (optional)
- `workerAIEndpoint` - Full HTTPS URL to the WorkerAI endpoint (optional)

---

### Initialization Methods

#### `begin()`

```cpp
bool begin()
```

Initializes the library and NVS storage. Must be called once in `setup()` before other methods.

**Returns:** `true` if successful, `false` otherwise

---

#### `setSkills()`

```cpp
bool setSkills(const char* skillsJson)
```

Sets the skills/commands configuration as a JSON string.  Call immediately following successful begin().

**Parameters:**
- `skillsJson` - JSON array defining available skills

**Returns:** `true` if JSON is valid, `false` otherwise

**Example JSON Structure:**

```json
[
  {
    "Target": "LED light",
    "Options": ["On", "Off", "Brightness"],
    "Actions": [
      {
        "Name": "On",
        "Type": "boolean",
        "Variants": ["turn on", "power on"]
      },
      {
        "Name": "Off",
        "Type": "boolean",
        "Variants": ["turn off", "power off"]
      },
      {
        "Name": "Brightness",
        "Type": "integer",
        "Variants": ["set to", "percent"]
      }
    ]
  }
]
```

---

### Configuration Methods

#### `configureI2S()`

```cpp
void configureI2S(int bckPin, int wsPin, int dataPin)
```

Configures the I2S pins for the microphone.

**Parameters:**
- `bckPin` - Bit Clock (BCK/SCK) pin
- `wsPin` - Word Select (WS/LRCLK) pin
- `dataPin` - Serial Data (SD/DATA) pin

---

#### `configureRecordingPin()`

```cpp
void configureRecordingPin(int pin, bool activeLow = false)
```

Configures the GPIO pin used to trigger recording.

**Parameters:**
- `pin` - GPIO pin number
- `activeLow` - Set to `true` if button connects pin to GND (default: `false`)

---

#### `setWorkerAIEndpoint()`

```cpp
void setWorkerAIEndpoint(const char* url)
```

Sets the Cloudflare WorkerAI API endpoint URL and saves it to NVS for persistence across reboots.

**Parameters:**
- `url` - Full HTTPS URL to the WorkerAI endpoint

**Note:** The endpoint URL is automatically loaded from NVS during `begin()` if previously saved.

---

### Advanced Configuration

You can customize recording time limits and block-based audio compression behavior by defining these constants **before** including the ESP32AI library in your sketch:

#### `MAX_RECORD_TIME_MS`

Maximum recording duration in milliseconds. Default is 3500ms (3.5 seconds).  This should be a safe value for most sketches.  For a device with 320KB RAM, increasing the recording time beyond 5 seconds will likely result in memory issues and potential allocation errors.

```cpp
#define MAX_RECORD_TIME_MS 5000  // Allow 5 second recordings
#include <ESP32AI.h>
```

#### `COMPRESSION_BLOCK_SIZE`

Number of samples per compression block. Default is 512 samples (~64ms at 8kHz). Each block is independently compressed based on its 95th percentile volume level for optimal transcription clarity.

```cpp
#define COMPRESSION_BLOCK_SIZE 1024  // Use 1KB blocks
#include <ESP32AI.h>
```

#### `COMPRESSION_TARGET_LEVEL`

Target level for the 95th percentile of each audio block (0-128 range for 8-bit audio). Default is 118 (92% of maximum).

```cpp
#define COMPRESSION_TARGET_LEVEL 115  // Slightly lower target to prevent clipping
#include <ESP32AI.h>
```

**Example: Custom configuration**

```cpp
// Define custom values BEFORE including the library
#define MAX_RECORD_TIME_MS 4000 // Increase audio buffer from ~28KB to ~32KB
#define COMPRESSION_BLOCK_SIZE 256 // More granular silence detection and vocal gain control
#define COMPRESSION_TARGET_LEVEL 120 // More aggressive gain

#include <WiFi.h>
#include <ESP32AI.h>

// Rest of your sketch...
```

**Note:** The library uses block-based compression that analyzes each chunk of audio independently. This provides real-time dynamic range compression, automatically adjusting gain for varying volume levels within a single recording. Blocks below the silence threshold are completely zeroed out, preventing noise amplification.

---

### Silence Threshold Calibration

The library includes an automatic calibration feature that measures your environment's ambient noise level and sets an appropriate silence threshold. **This is highly recommended** for optimal performance.

#### `calibrateSilenceThreshold()`

```cpp
bool calibrateSilenceThreshold(float multiplier = 3.0)
```

Calibrates the silence detection threshold by recording 3 seconds of ambient noise and analyzing the noise floor. The result is automatically saved to NVS (Non-Volatile Storage) and persists across reboots.

**Parameters:**
- `multiplier` - Multiplier applied to measured noise floor (default 3.0). Higher values = increased squelch.

**Usage:**

```cpp
void loop() {
    // Check for calibration command from Serial Monitor
    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'C' || cmd == 'c') {
            Serial.println("Starting calibration...");
            if (ai.calibrateSilenceThreshold()) {
                Serial.println("Calibration successful!");
            } else {
                Serial.println("Calibration failed!");
            }
        }
    }
    
    // Normal voice command processing...
    if (ai.startListening()) {
        // ...
    }
}
```

**Calibration Process:**

1. Ensure the environment is quiet (no talking, nominal background noise)
2. Send 'C' character via Serial Monitor
3. Library records 3 seconds of ambient noise
4. Calculates noise floor (95th percentile of absolute values)
5. Sets silence threshold to 3.0× the measured noise floor (configurable)
6. Saves threshold to NVS

**Custom Multiplier Examples:**

```cpp
// Use a more aggressive threshold (lower sensitivity to noise)
if (ai.calibrateSilenceThreshold(4.0)) {
    Serial.println("Calibrated with 4.0x multiplier");
}

// Use a tighter threshold (higher sensitivity, catches quieter sounds)
if (ai.calibrateSilenceThreshold(2.0)) {
    Serial.println("Calibrated with 2.0x multiplier");
}
```

**Recommended:** Run calibration once in your target environment. The threshold will be remembered after restarts.

---

### Recording & Processing

#### `startListening()`

```cpp
bool startListening()
```

Checks if the recording button is currently pressed. If pressed, records audio until the button is released or maximum recording time elapsed. If button is not pressed, returns immediately.

**Non-blocking design:** This function returns immediately if the button isn't pressed, allowing your loop() to continue running. Call it repeatedly in your loop().

**Returns:** `true` if audio was recorded successfully, `false` if button not pressed or recording failed

**Example:**

```cpp
void loop() {
    // Check for button press - returns immediately if not pressed
    if (ai.startListening()) {
        // Button was pressed and recording completed
        SkillResponse response = ai.processCommand();
        // Handle response...
    }
    
    // Loop continues - can do other tasks
    updateDisplay();
    checkSensors();
}
```

---

#### `processCommand()`

```cpp
SkillResponse processCommand(uint32_t timeoutMs = 12000)
```

Sends the recorded audio to WorkerAI and parses the response.

**Parameters:**
- `timeoutMs` - HTTP request timeout (default: 12000ms / 12 seconds)

**Returns:** `SkillResponse` struct containing the parsed action

---

### Response Structure

```cpp
struct SkillResponse {
    bool hasAction;           // true if an action was triggered
    String targetName;        // Name of the target (e.g., "Thermostat")
    String actionName;        // Name of the action (e.g., "Set")
    String actionValue;       // Value for the action (e.g., "72")
};
```

**Usage:**

```cpp
SkillResponse response = ai.processCommand();

if (response.hasAction) {
    Serial.printf("Target: %s\n", response.targetName.c_str());
    Serial.printf("Action: %s\n", response.actionName.c_str());
    if (response.actionValue.length() > 0) {
        Serial.printf("Value: %s\n", response.actionValue.c_str());
    }
}
```

---

### Utility Methods

#### `getDeviceID()`

```cpp
String getDeviceID() const
```

Returns the currently configured DeviceID.

---

#### `isReady()`

```cpp
bool isReady() const
```

Checks if the library is properly initialized.

**Returns:** `true` if ready to use, `false` otherwise

---

#### `runSetup()`

```cpp
void runSetup()
```

Interactive setup routine via Serial Monitor. Displays current NVS configuration and prompts for new values. Useful for updating credentials without recompiling your sketch.

**Parameters:** None

**Usage:**

```cpp
void loop() {
    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'S' || cmd == 's') {
            ai.runSetup();  // Launch interactive setup
        }
    }
}
```

**What it does:**
- Shows current DeviceID, Authorization status, and Endpoint URL
- Prompts for new values (press Enter to keep existing)
- Saves all changes to NVS

---

## Skills JSON Format

The skills JSON defines what voice commands your device understands. Each skill contains:

- **Target**: The name of the controllable function (e.g., "Thermostat", "LED")
- **Options**: Array of available actions (e.g., ["On", "Off", "Set"])
- **Actions**: Array of action definitions

Each **Action** contains:

- **Name**: The action identifier (must match an option)
- **Type**: Data type (`"boolean"`, `"integer"`, `"switch"`, `"string"`)
- **Variants**: Array of phrase variations that trigger this action

### Example: Complete Thermostat Skill

```json
[
  {
    "Target": "Thermostat",
    "Options": ["On", "Off", "Fan", "Heat", "Cool", "Set"],
    "Actions": [
      {
        "Name": "On",
        "Type": "boolean",
        "Variants": ["power on", "turn on"]
      },
      {
        "Name": "Off",
        "Type": "boolean",
        "Variants": ["power off", "turn off"]
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
        "Variants": ["cool to", "set ac to", "heat to", "set heat to"]
      }
    ]
  }
]
```

---

## WorkerAI API Format

### Request

The library sends a `multipart/form-data` POST request with:

1. **deviceId** (text field): The device identifier
2. **skills** (JSON field): The skills configuration
3. **audio** (binary field): WAV audio file (8kHz, 8-bit, mono PCM)

**Headers:**
- `Authorization`: The authorization token
- `Content-Type`: `multipart/form-data`

### Expected Response

The WorkerAI endpoint should return a JSON array:

```json
[
  {
    "Target": "Thermostat",
    "Action": "Set",
    "Value": "72"
  },
  {
    "Target": "LED",
    "Action": "None"
  }
]
```

**Rules:**
- Only **one** action per response should be non-empty
- Use `"None"`, `"null"`, or empty string for no action
- Include `"Value"` field for actions requiring parameters (e.g., `Set`)

---

## Examples

The library includes two examples:

### 1. BasicUsage

Simple light controller with on/off commands. Perfect for learning the basics.

**Location:** `examples/BasicUsage/BasicUsage.ino`

### 2. ThermostatControl

Advanced example with multiple actions and integer parameters. Demonstrates:
- More complex skill definitions
- Action value handling
- State management

**Location:** `examples/ThermostatControl/ThermostatControl.ino`

---

## Cloudflare Worker Setup

Both examples include a ready-to-deploy `worker.js` file that implements the voice processing backend using Cloudflare Workers AI.  Read the instructions contained in the worker.js files for instructions.

### What the Worker Does

- **Transcribes** audio using Whisper Tiny EN (speech-to-text)
- **Matches** a command to your device skills using keyword matching and if no match is found or the command is ambiguous, sends to Mistral 7B Instruct (AI understanding) for analysis
- **Returns** the matched action in the format ESP32AI expects

### Free Tier Limits

- 100,000 requests/day
- 10,000 AI operations/day
- ~3,500 voice commands/day with 3-second audio

---

## Troubleshooting

### Common Beginner Issues

#### "I don't see any output in Serial Monitor"
- **Solution**: Make sure Serial Monitor is set to **115200 baud** (bottom-right corner)
- Check that you've opened Serial Monitor **after** uploading the sketch

#### "My microphone doesn't work"
- **Check wiring**: Most common issue is swapped BCK/WS pins
  - INMP441: SCK→BCK pin, WS→WS pin, SD→DATA pin, L/R→GND
  - Power: VDD→3.3V (NOT 5V!), GND→GND
- **Check microphone LED**: Some boards have a power indicator LED
- **Test with calibration**: Send 'C' in Serial Monitor - it should record 3 seconds

#### "Button does nothing when pressed"
- **Check `activeLow` setting**: 
  - If button connects to GND: use `true`
  - If button connects to 3.3V: use `false`  
- **Test button separately**: Add `Serial.println(digitalRead(BUTTON_PIN));` in loop()

#### "Voice commands don't work / Nothing recognized"
- **Run calibration first**: Send 'C' in Serial Monitor, wait 3 seconds in quiet room
- **Speak clearly and close**: 3-6 inches from microphone
- **Check Worker is deployed**: Open the Worker URL in a browser - should show "Method Not Allowed" (this is correct!)
- **Check your variants**: Make sure your spoken phrase matches a variant in your skills JSON exactly

#### "HTTP Error -1"
- **Most common issue**: WiFi signal too weak (move closer to router)
- **Second most common**: Endpoint URL is wrong (check for typos, must be `https://`)

### TLS/SSL Connection Fails (HTTP Error -1)

If you see `HTTP Error code: -1` with "Connection failed - possible SSL/TLS handshake error":

**Common Causes:**
1. **WiFi signal too weak** - Move closer to router or use external antenna
2. **Insufficient heap memory** - The SSL/TLS handshake requires ~35KB free heap
3. **Network issues** - DNS resolution failure or firewall blocking HTTPS
4. **Endpoint unreachable** - Verify the CloudFlare Worker URL is correct

**Solutions:**
1. Check WiFi signal strength: `WiFi.RSSI()` (should be > -70 dBm)
2. Reduce `MAX_RECORD_TIME_MS` in ESP32AI.h to free up memory
3. Verify endpoint URL starts with `https://`
4. Test endpoint accessibility from another device
5. Check serial monitor for heap memory warnings

**Common Error Messages:**
- `HTTP Error code: -1` = TLS handshake failure or connection refused
- `HTTP Error code: -11` = Read timeout (server didn't respond in time)
- `HTTP Error code: -8` = Too little RAM (reduce `MAX_RECORD_TIME_MS` in ESP32AI.h)

### No action detected in response

- Verify WorkerAI endpoint returns proper JSON format
- Check skills JSON matches expected actions
- Ensure response contains exactly one non-"None" action
- Open the "Observability" tab in your Worker and review logs

### Credentials not persisting

- Call `begin()` after creating the ESP32AI instance
- Check that credentials and endpoint URL are not empty strings
- Verify NVS partition is available (check partition table)
- Endpoint URL is saved when `setWorkerAIEndpoint()` is called

---

## Dependencies

- **ArduinoJson** (v7.4.2 or later) - JSON parsing and serialization
- **esp32 by Espressif Systems** - For WiFi, I2S, NVS, and HTTP client

---

## License

This library is released under the MIT License. See the LICENSE file for details.
