/**
 * ESP32AI - AI-powered voice command library for ESP32
 * 
 * Copyright (c) 2026 Xorlent
 * Licensed under the MIT License.
 * https://github.com/Xorlent/ESP32AI
 * 
 * This library enables ESP32 devices to process voice commands using
 * Cloudflare WorkerAI and an I2S microphone
 * 
 */

#include "ESP32AI.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// NVS namespace for storing credentials
#define NVS_NAMESPACE "esp32ai"
#define NVS_DEVICEID "deviceid"
#define NVS_AUTH_KEY "auth"
#define NVS_ENDPOINT_KEY "endpoint"
#define NVS_SILENCE_KEY "silence"

/**
 * Constructor
 */
ESP32AI::ESP32AI(const char* deviceID, const char* authorization, const char* workerAIEndpoint) 
    : _i2sBckPin(-1), _i2sWsPin(-1), _i2sDataPin(-1), _i2sPort(I2S_NUM_0),
      _recordingPin(-1), _recordingActiveLow(false), _buttonWasPressed(false),
      _initialized(false), _i2sConfigured(false),
      _audioBuffer(nullptr), _audioBufferSize(0), _maxBufferSize(0),
      _multipartHeaderSpace(0), _audioDataOffset(0),
      _blockBuffer24(nullptr), _blockAbsValues(nullptr),
      _silenceThreshold(DEFAULT_SILENCE_THRESHOLD) {
    
    // If credentials provided, store them
    if (deviceID != nullptr) {
        _deviceID = String(deviceID);
    }
    if (authorization != nullptr) {
        _authorization = String(authorization);
    }
    if (workerAIEndpoint != nullptr) {
        String url = String(workerAIEndpoint);
        if (validateHttpsUrl(url)) {
            _workerAIEndpoint = normalizeEndpointUrl(url);
        } else {
            Serial.println("[ESP32AI] ERROR: Only HTTPS endpoints are allowed for security.");
            Serial.printf("[ESP32AI] Invalid URL: %s\n", workerAIEndpoint);
            _workerAIEndpoint = "";  // Clear invalid endpoint
        }
    }
}

/**
 * Destructor
 */
ESP32AI::~ESP32AI() {
    deinitializeI2S();
    if (_audioBuffer != nullptr) {
        free(_audioBuffer);
        _audioBuffer = nullptr;
    }
    if (_blockBuffer24 != nullptr) {
        free(_blockBuffer24);
        _blockBuffer24 = nullptr;
    }
    if (_blockAbsValues != nullptr) {
        free(_blockAbsValues);
        _blockAbsValues = nullptr;
    }
}

/**
 * Initialize the library
 */
bool ESP32AI::begin() {
    if (!initializeNVS()) {
        Serial.println("[ESP32AI] Failed to initialize ESP32AI.  Possible causes:");
        Serial.println("[ESP32AI]   - NVS (flash storage) initialization failed");
        Serial.println("[ESP32AI]   - Corrupted flash partition");
        Serial.println("[ESP32AI] Try erasing flash: Tools > Erase Flash > All Flash Contents");
        return false;
    }
    
    _initialized = true;
    Serial.println("[ESP32AI] Initialized successfully");
    
    // Check if credentials are configured
    bool needsSetup = (_deviceID.length() == 0 || 
                       _authorization.length() == 0 || 
                       _workerAIEndpoint.length() == 0);
    
    if (needsSetup) {
        Serial.println("[ESP32AI] ========================================");
        Serial.println("[ESP32AI] FIRST TIME SETUP REQUIRED");
        Serial.println("[ESP32AI] ========================================");
        Serial.println("[ESP32AI] No credentials found in NVS.");
        Serial.println("[ESP32AI] Starting interactive configuration...");
        Serial.println();
        
        // Automatically enter setup mode
        runSetup();
        
        Serial.println("[ESP32AI] Setup complete! Continuing initialization...");
    }
    
    return true;
}

/**
 * Initialize NVS and load/save credentials
 */
bool ESP32AI::initializeNVS() {
    if (!_preferences.begin(NVS_NAMESPACE, false)) {
        return false;
    }
    
    // If new credentials were provided, save them
    if (_deviceID.length() > 0) {
        _preferences.putString(NVS_DEVICEID, _deviceID);
        Serial.println("[ESP32AI] DeviceID saved to NVS");
    } else {
        // Load from NVS
        _deviceID = _preferences.getString(NVS_DEVICEID, "");
        if (_deviceID.length() == 0) {
            Serial.println("[ESP32AI] Warning: No DeviceID found in NVS");
        } else {
            Serial.println("[ESP32AI] DeviceID loaded from NVS");
        }
    }
    
    if (_authorization.length() > 0) {
        _preferences.putString(NVS_AUTH_KEY, _authorization);
        Serial.println("[ESP32AI] Authorization saved to NVS");
    } else {
        // Load from NVS
        _authorization = _preferences.getString(NVS_AUTH_KEY, "");
        if (_authorization.length() == 0) {
            Serial.println("[ESP32AI] Warning: No Authorization found in NVS");
        } else {
            Serial.println("[ESP32AI] Authorization loaded from NVS");
        }
    }
    
    // Save or load WorkerAI endpoint
    if (_workerAIEndpoint.length() > 0) {
        _preferences.putString(NVS_ENDPOINT_KEY, _workerAIEndpoint);
        Serial.println("[ESP32AI] Endpoint URL saved to NVS");
    } else {
        _workerAIEndpoint = _preferences.getString(NVS_ENDPOINT_KEY, "");
        if (_workerAIEndpoint.length() > 0) {
            // Validate loaded endpoint
            if (!validateHttpsUrl(_workerAIEndpoint)) {
                Serial.println("[ESP32AI] WARNING: Endpoint loaded from NVS is not HTTPS");
                Serial.printf("[ESP32AI] Invalid URL: %s\n", _workerAIEndpoint.c_str());
                _workerAIEndpoint = "";  // Clear invalid endpoint
            } else {
                Serial.println("[ESP32AI] Endpoint URL loaded from NVS");
            }
        }
    }
    
    // Load silence threshold from NVS (or use default if not calibrated)
    _silenceThreshold = _preferences.getInt(NVS_SILENCE_KEY, DEFAULT_SILENCE_THRESHOLD);
    if (_silenceThreshold != DEFAULT_SILENCE_THRESHOLD) {
        Serial.printf("[ESP32AI] Silence threshold loaded from NVS: %d\n", _silenceThreshold);
    }
    
    _preferences.end();
    return true;
}

/**
 * Set skills JSON configuration
 */
bool ESP32AI::setSkills(const char* skillsJson) {
    if (skillsJson == nullptr) {
        Serial.println("[ESP32AI] Error: Skills JSON is empty or null");
        return false;
    }
    
    // Prevent changing skills after they've been set
    if (_skillsJson.length() > 0) {
        Serial.println("[ESP32AI] Error: Skills already configured");
        Serial.println("[ESP32AI] Skills can only be set once during initialization");
        return false;
    }
    
    // Validate JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, skillsJson);
    
    if (error) {
        Serial.println("[ESP32AI] Failed to set skills.");
        Serial.println("[ESP32AI] Check for missing or misplaced quotes, brackets, commas");
        Serial.print("[ESP32AI] JSON error details: ");
        Serial.println(error.c_str());
        Serial.println("[ESP32AI] Check the JSON format and try again.");
        return false;
    }
    
    // Store the skills JSON
    _skillsJson = String(skillsJson);
    
    Serial.println("[ESP32AI] Skills configured successfully");
    return true;
}

/**
 * Configure I2S pins
 */
void ESP32AI::configureI2S(int bckPin, int wsPin, int dataPin) {
    _i2sBckPin = bckPin;
    _i2sWsPin = wsPin;
    _i2sDataPin = dataPin;
    Serial.printf("[ESP32AI] I2S configured: BCK=%d, WS=%d, DATA=%d\n", bckPin, wsPin, dataPin);
}

/**
 * Configure recording trigger pin
 */
void ESP32AI::configureRecordingPin(int pin, bool activeLow) {
    _recordingPin = pin;
    _recordingActiveLow = activeLow;
    pinMode(_recordingPin, activeLow ? INPUT_PULLUP : INPUT);
    Serial.printf("[ESP32AI] Recording pin configured: GPIO%d (%s)\n", 
                  pin, activeLow ? "active low" : "active high");
    
    // System is ready for use
    Serial.println("\n=== Ready! Press and hold button to record ===");
    Serial.println("=== Send 'C' to calibrate silence threshold ===");
    Serial.print("=== Send 'S' to update configuration ===\n\n");
}

/**
 * Set WorkerAI endpoint URL
 */
void ESP32AI::setWorkerAIEndpoint(const char* url) {
    if (url != nullptr && strlen(url) > 0) {
        String urlStr = String(url);
        
        // Validate HTTPS
        if (!validateHttpsUrl(urlStr)) {
            Serial.println("[ESP32AI] ERROR: Only HTTPS endpoints are valid");
            Serial.printf("[ESP32AI] Invalid URL: %s\n", url);
            return;
        }
        
        // Normalize the URL to ensure it ends with /api/process
        _workerAIEndpoint = normalizeEndpointUrl(urlStr);
        
        // Save to NVS for persistence
        if (_preferences.begin(NVS_NAMESPACE, false)) {
            _preferences.putString(NVS_ENDPOINT_KEY, _workerAIEndpoint);
            _preferences.end();
            Serial.printf("[ESP32AI] WorkerAI endpoint set and saved to NVS: %s\n", _workerAIEndpoint.c_str());
        } else {
            Serial.printf("[ESP32AI] WorkerAI endpoint set: %s (NVS save failed)\n", _workerAIEndpoint.c_str());
        }
    }
}

/**
 * Set DeviceID and save to NVS
 */
bool ESP32AI::setDeviceID(const char* deviceID) {
    if (deviceID == nullptr || strlen(deviceID) == 0) {
        Serial.println("[ESP32AI] Error: DeviceID cannot be empty");
        return false;
    }
    
    _deviceID = String(deviceID);
    
    // Save to NVS for persistence
    if (_preferences.begin(NVS_NAMESPACE, false)) {
        _preferences.putString(NVS_DEVICEID, _deviceID);
        _preferences.end();
        Serial.printf("[ESP32AI] DeviceID set and saved to NVS: %s\n", deviceID);
        return true;
    } else {
        Serial.println("[ESP32AI] Error: Failed to save DeviceID to NVS");
        return false;
    }
}

/**
 * Set Authorization token and save to NVS
 */
bool ESP32AI::setAuthorization(const char* authorization) {
    if (authorization == nullptr || strlen(authorization) == 0) {
        Serial.println("[ESP32AI] Error: Authorization token cannot be empty");
        return false;
    }
    
    _authorization = String(authorization);
    
    // Save to NVS for persistence
    if (_preferences.begin(NVS_NAMESPACE, false)) {
        _preferences.putString(NVS_AUTH_KEY, _authorization);
        _preferences.end();
        Serial.println("[ESP32AI] Authorization token set and saved to NVS");
        return true;
    } else {
        Serial.println("[ESP32AI] Error: Failed to save authorization token to NVS");
        return false;
    }
}

/**
 * Initialize I2S for recording
 */
bool ESP32AI::initializeI2S() {
    if (_i2sBckPin < 0 || _i2sWsPin < 0 || _i2sDataPin < 0) {
        Serial.println("[ESP32AI] Error: I2S pins not configured");
        return false;
    }
    
    if (_i2sConfigured) {
        return true; // Already configured
    }
    
    // Configuration tested with INMP441 microphone in I2S mode, 4k DMA buffer
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = DEFAULT_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,//I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    
    i2s_pin_config_t pin_config = {
        .bck_io_num = _i2sBckPin,
        .ws_io_num = _i2sWsPin,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = _i2sDataPin
    };
    
    esp_err_t err = i2s_driver_install(_i2sPort, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[ESP32AI] Error installing I2S driver: %d\n", err);
        return false;
    }
    
    err = i2s_set_pin(_i2sPort, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[ESP32AI] Error setting I2S pins: %d\n", err);
        i2s_driver_uninstall(_i2sPort);
        return false;
    }
    
    _i2sConfigured = true;
    Serial.println("[ESP32AI] I2S initialized successfully");
    return true;
}

/**
 * Deinitialize I2S
 */
void ESP32AI::deinitializeI2S() {
    if (_i2sConfigured) {
        i2s_driver_uninstall(_i2sPort);
        _i2sConfigured = false;
        Serial.println("[ESP32AI] I2S deinitialized");
    }
}

/**
 * Start listening for voice commands
 */
bool ESP32AI::startListening() {
    if (!_initialized) {
        Serial.println("[ESP32AI] Error: Library not initialized. Call begin() first");
        return false;
    }
    
    if (_recordingPin < 0) {
        Serial.println("[ESP32AI] Error: Recording pin not configured");
        return false;
    }
    
    if (!initializeI2S()) {
        return false;
    }
    
    // Check if button is currently pressed (non-blocking)
    bool pinActive = _recordingActiveLow ? LOW : HIGH;
    bool buttonPressed = (digitalRead(_recordingPin) == pinActive);
    
    if (!buttonPressed) {
        // Button is released - update state and return
        if (_buttonWasPressed) {
            _buttonWasPressed = false;
            Serial.println("[ESP32AI] Button was released, ready to record");
        }
        return false;
    }
    
    // Button is pressed - check if this is a NEW press (transition from released to pressed)
    if (_buttonWasPressed) {
        // Button is still pressed from previous recording - ignore to prevent rapid re-recording
        // (Will be ready for new recording once button is released and pressed again)
        return false;
    }
    
    // This is a new button press - start recording
    _buttonWasPressed = true;  // Mark button as pressed to prevent re-trigger
    
    Serial.println("[ESP32AI] Recording started...");
    
    // Record audio
    bool success = recordAudio();
    
    if (success) {
        size_t audioDataSize = _audioBufferSize - _audioDataOffset;
        Serial.printf("[ESP32AI] Recording complete: %d bytes audio data\n", audioDataSize);
        
        // Keep button pressed state - next startListening() will wait for release before new recording
        // This prevents immediate re-recording if button is still held down
    } else {
        Serial.println("[ESP32AI] Recording failed");
        _buttonWasPressed = false;  // Reset state on failure
    }
    
    return success;
}

/**
 * Calibrate silence threshold by measuring ambient noise
 */
bool ESP32AI::calibrateSilenceThreshold(float multiplier) {
    if (!_initialized) {
        Serial.println("[ESP32AI] Error: Library not initialized. Call begin() first");
        return false;
    }
    
    if (!initializeI2S()) {
        return false;
    }
    
    // Free audio buffer to ensure sufficient memory for calibration
    // The buffer will be reallocated on next recording
    if (_audioBuffer != nullptr) {
        Serial.println("[ESP32AI] Freeing audio buffer for calibration");
        free(_audioBuffer);
        _audioBuffer = nullptr;
        _maxBufferSize = 0;
        _multipartHeaderSpace = 0;
        _audioDataOffset = 0;
        _audioBufferSize = 0;
    }
    
    Serial.println("[ESP32AI] ========================================");
    Serial.println("[ESP32AI]           SILENCE CALIBRATION");
    Serial.println("[ESP32AI] ========================================");
    Serial.println("[ESP32AI] Please remain QUIET for 3 seconds...");
    Serial.println();
    
    delay(1000);  // Give user time to read the message
    
    // Flush any stale data from I2S FIFO
    int32_t discardBuffer[128];
    size_t bytesRead = 0;
    for (int i = 0; i < 5; i++) {
        i2s_read(_i2sPort, discardBuffer, sizeof(discardBuffer), &bytesRead, 10);
    }
    
    // Record 3 seconds of ambient noise
    const uint32_t calibrationDuration = 3000;  // 3 seconds
    const size_t samplesPerRead = 256;
    const size_t totalSamples = (DEFAULT_SAMPLE_RATE * calibrationDuration) / 1000;
    
    // Allocate temporary buffer for absolute values
    int32_t* absoluteValues = (int32_t*)malloc(totalSamples * sizeof(int32_t));
    if (absoluteValues == nullptr) {
        Serial.println("[ESP32AI] Error: Failed to allocate calibration buffer");
        return false;
    }
    
    size_t sampleCount = 0;
    unsigned long startTime = millis();
    int32_t i2s_buffer[samplesPerRead];
    
    // Collect absolute values of all samples
    while (sampleCount < totalSamples && (millis() - startTime) < calibrationDuration) {
        bytesRead = 0;
        size_t samplesToRead = min(samplesPerRead, totalSamples - sampleCount);
        
        // Use 100ms timeout instead of portMAX_DELAY
        esp_err_t result = i2s_read(_i2sPort, i2s_buffer, samplesToRead * sizeof(int32_t), 
                                    &bytesRead, pdMS_TO_TICKS(100));
        
        if (result == ESP_OK && bytesRead > 0) {
            size_t samplesRead = bytesRead / sizeof(int32_t);
            
            for (size_t i = 0; i < samplesRead && sampleCount < totalSamples; i++) {
                int32_t sample24 = i2s_buffer[i] >> 8;  // Get 24-bit value
                absoluteValues[sampleCount++] = abs(sample24);
            }
        }
    }
    
    if (sampleCount < totalSamples / 2) {
        Serial.println("[ESP32AI] Error: Insufficient calibration data collected");
        free(absoluteValues);
        return false;
    }
    
    // Verify proper microphone function by examining data variation
    int32_t minVal = absoluteValues[0];
    int32_t maxVal = absoluteValues[0];
    for (size_t i = 1; i < sampleCount; i++) {
        if (absoluteValues[i] < minVal) minVal = absoluteValues[i];
        if (absoluteValues[i] > maxVal) maxVal = absoluteValues[i];
    }
    
    // Check if datastream is empty
    if (maxVal == 0) {
        Serial.println("[ESP32AI] Error: No audio signal detected");
        Serial.println("[ESP32AI] 1. Microphone not connected");
        Serial.println("[ESP32AI] 2. Wrong I2S pins configured (is the L/R pin grounded?)");
        Serial.println("[ESP32AI] 3. Faulty hardware");
        free(absoluteValues);
        return false;
    }
    
    // Check for insufficient variation (stuck/dead microphone)
    int32_t range = maxVal - minVal;
    if (range < 100) {  // Threshold for minimum expected variation (24-bit scale)
        Serial.println("[ESP32AI] Error: Insufficient audio variation detected");
        Serial.println("[ESP32AI] 1. Microphone not functioning");
        Serial.println("[ESP32AI] 2. Microphone input shorted/grounded");
        Serial.println("[ESP32AI] 3. Faulty hardware");
        free(absoluteValues);
        return false;
    }
    
    Serial.printf("[ESP32AI] Microphone check passed (variation range: %d)\n", range);
    
    // Calculate 95th percentile of ambient noise
    int32_t noiseFloor = calculate95thPercentile(absoluteValues, sampleCount);
    free(absoluteValues);
    
    // Set threshold to multiplier × noise floor (provides safety margin)
    _silenceThreshold = noiseFloor * multiplier;
    
    // Save to NVS for persistence
    if (_preferences.begin(NVS_NAMESPACE, false)) {
        _preferences.putInt(NVS_SILENCE_KEY, _silenceThreshold);
        _preferences.end();
    }
    
    Serial.println();
    Serial.println("[ESP32AI] ========================================");
    Serial.println("[ESP32AI]          CALIBRATION COMPLETE");
    Serial.println("[ESP32AI] ========================================");
    Serial.printf("[ESP32AI] Noise floor (95th percentile): %d\n", noiseFloor);
    Serial.printf("[ESP32AI] Multiplier applied: %.1fx\n", multiplier);
    Serial.printf("[ESP32AI] Silence threshold set to: %d\n", _silenceThreshold);
    Serial.println("[ESP32AI] Threshold saved to NVS (persistent)");
    Serial.println("[ESP32AI] ========================================");
    Serial.println();
    
    return true;
}

/**
 * Helper function to read a line from Serial with timeout
 */
String ESP32AI::readSerialLine(unsigned long timeoutMs) {
    String input = "";
    unsigned long startTime = millis();
    
    while (millis() - startTime < timeoutMs) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') {
                // Newline received - return whatever we have (could be empty)
                return input;
            } else if (c != '\r') {
                // Add character (ignore carriage returns)
                input += c;
            }
        }
        delay(10);
    }
    return input;
}

/**
 * Interactive setup routine - configure NVS settings
 */
void ESP32AI::runSetup() {
    Serial.println("\n========================================");
    Serial.println("       ESP32AI CONFIGURATION SETUP      ");
    Serial.println("========================================\n");
    
    // Clear serial buffer
    delay(50);
    while (Serial.available()) {
        Serial.read();
    }
    
    // Display current configuration
    Serial.println("Current Configuration:");
    Serial.println("---------------------");
    
    if (_deviceID.length() > 0) {
        Serial.printf("Device ID:      %s\n", _deviceID.c_str());
    } else {
        Serial.println("Device ID:      <not configured>");
    }
    
    if (_authorization.length() > 0) {
        Serial.println("Authorization:  <configured>");
    } else {
        Serial.println("Authorization:  <not configured>");
    }
    
    if (_workerAIEndpoint.length() > 0) {
        Serial.printf("Endpoint URL:   %s\n", _workerAIEndpoint.c_str());
    } else {
        Serial.println("Endpoint URL:   <not configured>");
    }
    
    Serial.println("\n---------------------");
    Serial.println("Enter new values or press ENTER to keep existing values.");
    Serial.println("Note: Fields without an existing value must be provided.\n");
    
    // Prompt for Device ID
    String newDeviceID = "";
    while (true) {
        if (_deviceID.length() > 0) {
            Serial.printf("Device ID [%s]: ", _deviceID.c_str());
        } else {
            Serial.print("Device ID (required): ");
        }
        
        newDeviceID = readSerialLine(30000);
        
        // If empty and we have a previous value, keep it
        if (newDeviceID.length() == 0 && _deviceID.length() > 0) {
            newDeviceID = _deviceID;
            Serial.println("  (Keeping existing value)");
            break;
        }
        
        // If empty and no previous value, re-prompt
        if (newDeviceID.length() == 0) {
            Serial.println("  Error: Device ID is required. Please enter a value.");
            continue;
        }
        
        // Valid input provided
        break;
    }
    
    // Prompt for Authorization Token
    String newAuthorization = "";
    bool hasAuth = _authorization.length() > 0;
    while (true) {
        if (hasAuth) {
            Serial.print("Authorization Token [<configured>]: ");
        } else {
            Serial.print("Authorization Token (required): ");
        }
        
        newAuthorization = readSerialLine(30000);
        
        // If empty and we have a previous value, skip updating
        if (newAuthorization.length() == 0 && hasAuth) {
            Serial.println("  (Keeping existing value)");
            newAuthorization = "";  // Skip update as though user hit ENTER without typing anything
            break;
        }
        
        // If empty and no previous value, re-prompt
        if (newAuthorization.length() == 0) {
            Serial.println("  Error: Authorization token is required. Please enter a value.");
            continue;
        }
        
        // Valid input provided
        break;
    }
    
    // Prompt for Endpoint URL
    String newEndpoint = "";
    while (true) {
        if (_workerAIEndpoint.length() > 0) {
            Serial.printf("Endpoint URL [%s]: ", _workerAIEndpoint.c_str());
        } else {
            Serial.print("Endpoint URL (required, must be HTTPS): ");
        }
        
        newEndpoint = readSerialLine(30000);
        
        // If empty and we have a previous value, keep it
        if (newEndpoint.length() == 0 && _workerAIEndpoint.length() > 0) {
            newEndpoint = _workerAIEndpoint;
            Serial.println("  (Keeping existing value)");
            break;
        }
        
        // If empty and no previous value, re-prompt
        if (newEndpoint.length() == 0) {
            Serial.println("  Error: Endpoint URL is required. Please enter a value.");
            continue;
        }
        
        // Validate HTTPS
        if (!newEndpoint.startsWith("https://")) {
            Serial.println("  Error: Endpoint must use HTTPS. Please enter a valid HTTPS URL.");
            continue;
        }
        
        // Valid input provided
        break;
    }
    
    // Save the configuration
    Serial.println("\nSaving configuration to NVS...");
    
    bool success = true;
    
    if (newDeviceID != _deviceID) {
        if (!setDeviceID(newDeviceID.c_str())) {
            success = false;
        }
    }
    
    if (newAuthorization.length() > 0) {  // Only update if new value provided
        if (!setAuthorization(newAuthorization.c_str())) {
            success = false;
        }
    }
    
    if (newEndpoint != _workerAIEndpoint) {
        setWorkerAIEndpoint(newEndpoint.c_str());
    }
    
    Serial.println("\n========================================");
    if (success) {
        Serial.println("  Configuration saved successfully!");
    } else {
        Serial.println("  Configuration saved with errors.");
    }
    Serial.println("========================================\n");
}

/**
 * Calculate the space needed for multipart headers based on actual content sizes
 * This includes: boundaries, deviceID field, skills JSON field, audio field header, and WAV header
 */
size_t ESP32AI::calculateHeaderSpace() {
    size_t size = 0;
    
    // Boundary string is 25 characters (----ESP32AIBoundary12345)
    size_t boundaryLen = 25;
    
    // DeviceID field
    // --boundary\r\n
    size += 2 + boundaryLen + 2;
    // Content-Disposition: form-data; name="deviceId"\r\n\r\n
    size += 51;
    // Actual deviceID value
    size += _deviceID.length();
    // \r\n
    size += 2;
    
    // Skills JSON field  
    // --boundary\r\n
    size += 2 + boundaryLen + 2;
    // Content-Disposition: form-data; name="skills"\r\n
    size += 49;
    // Content-Type: application/json\r\n\r\n
    size += 32;
    // Actual skills JSON
    size += _skillsJson.length();
    // \r\n
    size += 2;
    
    // Audio field header
    // --boundary\r\n
    size += 2 + boundaryLen + 2;
    // Content-Disposition: form-data; name="audio"; filename="audio.wav"\r\n
    size += 71;
    // Content-Type: audio/wav\r\n\r\n
    size += 26;
    
    // WAV header (44 bytes)
    size += 44;
    
    // Safety margin
    size += 8;
    
    return size;
}

/**
 * Record audio from I2S microphone with per-block compression
 */
bool ESP32AI::recordAudio() {
    // Calculate audio data size for maximum recording time
    // Round up to next block boundary for clean alignment
    size_t samplesForDuration = (DEFAULT_SAMPLE_RATE * DEFAULT_CHANNELS * MAX_RECORD_TIME_MS) / 1000;
    size_t blocksNeeded = (samplesForDuration + COMPRESSION_BLOCK_SIZE - 1) / COMPRESSION_BLOCK_SIZE;
    size_t maxAudioDataSize = blocksNeeded * COMPRESSION_BLOCK_SIZE * (DEFAULT_BITS_PER_SAMPLE / 8);
    
    bool buffersAllocated = false;  // Track if we allocated this call
    
    // Calculate required buffer size including space for multipart headers and footer
    if (_audioBuffer == nullptr) {
        _multipartHeaderSpace = calculateHeaderSpace();
        _audioDataOffset = _multipartHeaderSpace;
        
        size_t multipartFooterSpace = 64;  // Space for closing boundary
        size_t requiredBufferSize = _multipartHeaderSpace + maxAudioDataSize + multipartFooterSpace;
        
        // Check available heap
        size_t freeHeap = ESP.getFreeHeap();
        if (requiredBufferSize > freeHeap / 2) {
            Serial.printf("[ESP32AI] Warning: Buffer size (%d bytes) may be tight. Free heap: %d bytes\n", 
                          requiredBufferSize, freeHeap);
        }
        
        _audioBuffer = (uint8_t*)malloc(requiredBufferSize);
        if (_audioBuffer == nullptr) {
            Serial.printf("[ESP32AI] Error: Failed to allocate audio buffer (%d bytes)\n", requiredBufferSize);
            Serial.printf("[ESP32AI] Free heap: %d bytes\n", ESP.getFreeHeap());
            _maxBufferSize = 0;
            _multipartHeaderSpace = 0;
            _audioDataOffset = 0;
            return false;
        }
        _maxBufferSize = requiredBufferSize;
        Serial.printf("[ESP32AI] Unified buffer allocated successfully, free heap: %d bytes\n", ESP.getFreeHeap());
        buffersAllocated = true;
    } else if (_maxBufferSize < (_multipartHeaderSpace + maxAudioDataSize + 64)) {
        // Buffer exists but is too small - reallocate
        Serial.println("[ESP32AI] Buffer too small, reallocating...");
        free(_audioBuffer);
        _audioBuffer = nullptr;
        _multipartHeaderSpace = 0;
        _audioDataOffset = 0;
        _maxBufferSize = 0;
        // Will be reallocated below
    }
    
    // Allocate block processing buffers only if not allocated
    if (_blockBuffer24 == nullptr) {
        _blockBuffer24 = (int32_t*)malloc(COMPRESSION_BLOCK_SIZE * sizeof(int32_t));
        if (_blockBuffer24 == nullptr) {
            Serial.println("[ESP32AI] Error: Failed to allocate block processing buffer");
            return false;
        }
        buffersAllocated = true;
    }
    
    if (_blockAbsValues == nullptr) {
        _blockAbsValues = (int32_t*)malloc(COMPRESSION_BLOCK_SIZE * sizeof(int32_t));
        if (_blockAbsValues == nullptr) {
            Serial.println("[ESP32AI] Error: Failed to allocate absolute values buffer");
            return false;
        }
        buffersAllocated = true;
    }
    
    // Reset audio buffer position to just after header space (audio will be written here)
    _audioBufferSize = _audioDataOffset;
    unsigned long recordStart = millis();
    bool pinActive = _recordingActiveLow ? LOW : HIGH;
    
    Serial.printf("[ESP32AI] Using block-based compression (block size: %d samples)\n", COMPRESSION_BLOCK_SIZE);
    Serial.printf("[ESP32AI] Silence threshold: %d\n", _silenceThreshold);
    
    // Flush any stale data from I2S FIFO by reading and discarding
    int32_t discardBuffer[128];
    size_t bytesRead = 0;
    for (int i = 0; i < 5; i++) {
        i2s_read(_i2sPort, discardBuffer, sizeof(discardBuffer), &bytesRead, 10);
    }
    
    Serial.println("[ESP32AI] I2S buffers cleared, starting fresh recording...");
    
    size_t blockPosition = 0;  // Current position within the block
    int activeBlocks = 0;      // Count of blocks with actual audio (for statistics)
    int silentBlocks = 0;      // Count of silent blocks (for statistics)
    
    // Calculate maximum audio capacity (reserve footer space)
    // Footer needs: "\r\n--boundary--\r\n" = ~33 bytes, plus margin
    size_t footerReserve = 64;
    size_t maxAudioEnd = _maxBufferSize - footerReserve;
    
    // Record while pin is pressed and time limit not exceeded
    while ((millis() - recordStart) < MAX_RECORD_TIME_MS && _audioBufferSize < maxAudioEnd) {
        
        // Check button state with debounce for release detection
        if (digitalRead(_recordingPin) != pinActive) {
            // Button appears released - verify it's stable for 30ms to prevent bounce
            unsigned long debounceStart = millis();
            bool stableRelease = true;
            
            while (millis() - debounceStart < 30) {
                if (digitalRead(_recordingPin) == pinActive) {
                    // Button pressed again during debounce - false alarm, continue recording
                    stableRelease = false;
                    break;
                }
                delay(1);
            }
            
            if (stableRelease) {
                // Button truly released after debounce period
                // Process any remaining partial block (only occurs on early button release,
                // never at buffer capacity since buffer is block-aligned)
                if (blockPosition > 0) {
                    // Calculate 95th percentile for partial block
                    for (size_t i = 0; i < blockPosition; i++) {
                        _blockAbsValues[i] = abs(_blockBuffer24[i]);
                    }
                    int32_t blockLevel = calculate95thPercentile(_blockAbsValues, blockPosition);
                    // Note: _blockAbsValues is now modified by quickselect (no longer needed)
                    
                    // Check if this is a silent block
                    bool isSilent = blockLevel < _silenceThreshold;
                    
                    if (isSilent) {
                        // Silent block - zero out all samples
                        for (size_t i = 0; i < blockPosition && _audioBufferSize < maxAudioEnd; i++) {
                            _audioBuffer[_audioBufferSize++] = 128;  // 128 = silence in unsigned 8-bit
                        }
                        silentBlocks++;
                    } else {
                        // Active audio block - compress and convert
                        int32_t gainDivisor = blockLevel / COMPRESSION_TARGET_LEVEL;
                        if (gainDivisor < 1) gainDivisor = 1;  // Prevent division issues
                        
                        for (size_t i = 0; i < blockPosition && _audioBufferSize < maxAudioEnd; i++) {
                            int32_t sample8 = _blockBuffer24[i] / gainDivisor;
                            
                            // Clamp to 8-bit signed range
                            if (sample8 > 127) sample8 = 127;
                            if (sample8 < -128) sample8 = -128;
                            
                            // Convert signed to unsigned 8-bit
                            _audioBuffer[_audioBufferSize++] = (uint8_t)(sample8 + 128);
                        }
                        activeBlocks++;
                    }
                }
                break;
            }
            // Otherwise continue recording (bounce detected and ignored)
        }
        
        // Process filled block before reading more samples
        if (blockPosition >= COMPRESSION_BLOCK_SIZE) {
            // Calculate absolute values for percentile calculation
            for (size_t j = 0; j < COMPRESSION_BLOCK_SIZE; j++) {
                _blockAbsValues[j] = abs(_blockBuffer24[j]);
            }
            
            // Calculate 95th percentile of this block
            int32_t blockLevel = calculate95thPercentile(_blockAbsValues, COMPRESSION_BLOCK_SIZE);
            
            // Check if this is a silent block
            bool isSilent = blockLevel < _silenceThreshold;
            
            if (isSilent) {
                // Silent block - zero out all samples
                for (size_t j = 0; j < COMPRESSION_BLOCK_SIZE && _audioBufferSize < maxAudioEnd; j++) {
                    _audioBuffer[_audioBufferSize++] = 128;
                }
                silentBlocks++;
            } else {
                // Active audio block - compress and convert
                int32_t gainDivisor = blockLevel / COMPRESSION_TARGET_LEVEL;
                if (gainDivisor < 1) gainDivisor = 1;
                
                for (size_t j = 0; j < COMPRESSION_BLOCK_SIZE && _audioBufferSize < maxAudioEnd; j++) {
                    int32_t sample8 = _blockBuffer24[j] / gainDivisor;
                    
                    // Clamp to 8-bit signed range
                    if (sample8 > 127) sample8 = 127;
                    if (sample8 < -128) sample8 = -128;
                    
                    // Convert signed to unsigned 8-bit
                    _audioBuffer[_audioBufferSize++] = (uint8_t)(sample8 + 128);
                }
                activeBlocks++;
            }
            
            blockPosition = 0;  // Reset for next block
        }
        
        // Read samples from INMP441 into block buffer
        int32_t i2s_buffer[256];
        bytesRead = 0;
        size_t samplesToRead = min((size_t)256, (size_t)(COMPRESSION_BLOCK_SIZE - blockPosition));
        
        // Use 100ms timeout instead of portMAX_DELAY to prevent hang on hardware failure
        esp_err_t result = i2s_read(_i2sPort, i2s_buffer, samplesToRead * sizeof(int32_t), 
                                    &bytesRead, pdMS_TO_TICKS(100));
        
        if (result == ESP_OK && bytesRead > 0) {
            size_t samplesRead = bytesRead / sizeof(int32_t);
            
            // Add samples to block buffer
            for (size_t i = 0; i < samplesRead; i++) {
                _blockBuffer24[blockPosition++] = i2s_buffer[i] >> 8;  // Convert to 24-bit
            }
        }
    }
    
    // Check if any audio was recorded
    size_t audioDataSize = _audioBufferSize - _audioDataOffset;
    if (audioDataSize == 0) {
        Serial.println("[ESP32AI] Warning: No audio data recorded");
        return false;
    }
    
    Serial.printf("[ESP32AI] Recording complete: %d bytes audio data\n", audioDataSize);
    Serial.printf("[ESP32AI] Active blocks: %d, Silent blocks: %d\n", activeBlocks, silentBlocks);
    Serial.printf("[ESP32AI] Free heap: %d bytes\n", ESP.getFreeHeap());
    
    // Check if we hit the maximum recording time limit
    if ((millis() - recordStart) >= MAX_RECORD_TIME_MS) {
        Serial.printf("[ESP32AI] Recording truncated at %d seconds\n", MAX_RECORD_TIME_MS / 1000);
    }
    
    return true;
}

/**
 * Process the recorded command
 */
SkillResponse ESP32AI::processCommand(uint32_t timeoutMs) {
    SkillResponse response;
    
    if (!_initialized) {
        Serial.println("[ESP32AI] Error: Library not initialized");
        return response;
    }
    
    if (_audioBuffer == nullptr || _audioBufferSize <= _audioDataOffset) {
        Serial.println("[ESP32AI] Error: No audio data to process");
        return response;
    }
    
    if (_workerAIEndpoint.length() == 0) {
        Serial.println("[ESP32AI] Error: WorkerAI endpoint not configured");
        return response;
    }
    
    Serial.println("[ESP32AI] Sending audio to WorkerAI...");
    
    String jsonResponse = sendToWorkerAI(timeoutMs);
    
    if (jsonResponse.length() > 0) {
        Serial.println("[ESP32AI] Response received, parsing...");
        response = parseResponse(jsonResponse);
    } else {
        Serial.println("[ESP32AI] Error: Empty response from WorkerAI");
    }
    
    return response;
}

/**
 * Send audio data to WorkerAI endpoint
 */
String ESP32AI::sendToWorkerAI(uint32_t timeoutMs) {
    HTTPClient http;
    WiFiClientSecure *secureClient = nullptr;
    String response;
    
    // Check WiFi connection first
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ESP32AI] ERROR: WiFi not connected!");
        return "";
    }
    
    // Enforce HTTPS-only connections
    if (!validateHttpsUrl(_workerAIEndpoint)) {
        Serial.println("[ESP32AI] ERROR: Only HTTPS endpoints are allowed.");
        Serial.printf("[ESP32AI] Current endpoint: %s\n", _workerAIEndpoint.c_str());
        return "";
    }
    
    // Use WiFiClientSecure for HTTPS connections
    if (_workerAIEndpoint.startsWith("https://")) {
        secureClient = new WiFiClientSecure;
        if (secureClient) {
            // Configure SSL client for ESP32
            secureClient->setInsecure();  // Skip certificate verification
            
            // Set timeouts - be generous for SSL handshake
            secureClient->setTimeout(timeoutMs / 1000);  // Socket timeout in seconds
            
            // Set connection timeout for the initial TCP connection
            secureClient->setHandshakeTimeout(7);  // SSL handshake timeout in seconds
            
            // Connect with proper URL parsing
            if (!http.begin(*secureClient, _workerAIEndpoint)) {
                Serial.println("[ESP32AI] Failed HTTP connection");
                delete secureClient;
                return "";
            }
            http.setTimeout(timeoutMs);  // HTTP client timeout in milliseconds
            
            Serial.println("[ESP32AI] HTTPS client configured");
        } else {
            Serial.println("[ESP32AI] Failed to create WiFiClientSecure");
            return "";
        }
    } else {
        http.begin(_workerAIEndpoint);
        http.setTimeout(timeoutMs);  // Use provided timeout parameter
    }
    
    // Set headers
    if (_authorization.length() > 0) {
        http.addHeader("Authorization", _authorization);
    }
    
    // Create multipart boundary
    String boundary = "----ESP32AIBoundary" + String(random(10000, 99999));
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    
    // Check available memory before creating request body
    size_t freeHeap = ESP.getFreeHeap();
    size_t audioDataSize = _audioBufferSize - _audioDataOffset;
    size_t estimatedBodySize = _maxBufferSize;  // Entire buffer will be used for multipart body
    size_t requiredHeap = estimatedBodySize + 35000;  // Add 35KB for SSL, HTTP processing, and buffers
    
    if (freeHeap < requiredHeap) {
        Serial.printf("[ESP32AI] ERROR: Insufficient memory.\n");
        Serial.printf("[ESP32AI]   Free heap: %d bytes\n", freeHeap);
        Serial.printf("[ESP32AI]   Required: ~%d bytes\n", requiredHeap);
        Serial.println("[ESP32AI]   1. Reduce MAX_RECORD_TIME_MS in ESP32AI.h");
        Serial.println("[ESP32AI]   2. Use lower sample rate");
        Serial.println("[ESP32AI]   3. Record shorter audio clips");
        
        if (secureClient) {
            delete secureClient;
        }
        http.end();
        return "";
    }
    
    // Prepare multipart body in the audio buffer
    size_t totalSize = prepareMultipartBuffer(boundary);
    
    if (totalSize == 0) {
        Serial.println("[ESP32AI] Error: Failed to prepare multipart buffer");
        if (secureClient) {
            delete secureClient;
        }
        http.end();
        return "";
    }
    
    Serial.printf("[ESP32AI] Sending request (%d bytes) with %dms timeout...\n", 
                  totalSize, timeoutMs);
    
    // Retry logic for ESP32 SSL connections (first attempt often fails)
    int httpResponseCode = -1;
    int maxRetries = 2;  // Try up to 3 times total (1 initial + 2 retries)
    
    for (int attempt = 0; attempt <= maxRetries; attempt++) {
        if (attempt > 0) {
            Serial.printf("[ESP32AI] Retry attempt %d/%d...\n", attempt, maxRetries);
            delay(500);  // Brief delay between retries
        }
        
        httpResponseCode = http.POST(_audioBuffer, totalSize);
        
        // If we got any response code other than connection failure, break
        if (httpResponseCode != HTTPC_ERROR_CONNECTION_REFUSED) {
            break;
        }
        
        // On connection failure, log and retry if we have attempts left
        if (attempt < maxRetries) {
            Serial.println("[ESP32AI] Connection failed, retrying...");
        }
    }
    
    if (httpResponseCode > 0) {
        // Get response body for both success and error cases
        response = http.getString();
        
        // Check for success status codes (200-299)
        if (httpResponseCode >= 200 && httpResponseCode < 300) {
            // Success - response already captured above
        } else {
            Serial.printf("[ESP32AI] Server error: %d\n", httpResponseCode);
            Serial.printf("[ESP32AI] Server response: %s\n", response.c_str());
            
            if (httpResponseCode == 401) {
                Serial.println("[ESP32AI] Authentication failed - check authorization token");
            } else if (httpResponseCode == 400) {
                Serial.println("[ESP32AI] Bad request - check audio format and form data");
            } else if (httpResponseCode == 500) {
                Serial.println("[ESP32AI] Server error - check Cloudflare Worker logs");
                Serial.println("  1. Whisper model failed to process audio format");
                Serial.println("  2. Invalid skills JSON structure");
                Serial.println("  3. Missing environment bindings (AI binding)");
            }
            
            // Return empty string to trigger error handling
            response = "";
        }
    } else {
        Serial.printf("[ESP32AI] HTTP Error code: %d\n", httpResponseCode);
        
        // Provide more specific error messages
        switch (httpResponseCode) {
            case HTTPC_ERROR_CONNECTION_REFUSED:  // -1
                Serial.println("[ESP32AI] Connection failed - possible SSL/TLS handshake error");
                Serial.println("  1. Check endpoint URL is correct");
                Serial.println("  2. Verify WiFi signal is strong");
                Serial.println("  3. Ensure sufficient free heap memory");
                break;
            case HTTPC_ERROR_SEND_HEADER_FAILED:  // -2
                Serial.println("[ESP32AI] Failed to send HTTP headers");
                break;
            case HTTPC_ERROR_SEND_PAYLOAD_FAILED:  // -3
                Serial.println("[ESP32AI] Failed to send HTTP payload");
                break;
            case HTTPC_ERROR_NOT_CONNECTED:  // -4
                Serial.println("[ESP32AI] Not connected to server");
                break;
            case HTTPC_ERROR_CONNECTION_LOST:  // -5
                Serial.println("[ESP32AI] Connection lost during transfer");
                break;
            case HTTPC_ERROR_NO_STREAM:  // -6
                Serial.println("[ESP32AI] No stream available");
                break;
            case HTTPC_ERROR_NO_HTTP_SERVER:  // -7
                Serial.println("[ESP32AI] No HTTP server");
                break;
            case HTTPC_ERROR_TOO_LESS_RAM:  // -8
                Serial.println("[ESP32AI] Too little RAM available");
                break;
            case HTTPC_ERROR_ENCODING:  // -9
                Serial.println("[ESP32AI] Transfer encoding error");
                break;
            case HTTPC_ERROR_STREAM_WRITE:  // -10
                Serial.println("[ESP32AI] Stream write error");
                break;
            case HTTPC_ERROR_READ_TIMEOUT:  // -11
                Serial.println("[ESP32AI] Read timeout");
                break;
            default:
                Serial.println("[ESP32AI] Check network connection and endpoint URL");
                break;
        }
    }
    
    http.end();
    
    // Clean up the secure client if it was created
    if (secureClient) {
        delete secureClient;
    }
    
    return response;
}

/**
 * Prepare multipart/form-data directly in the audio buffer
 * The audio data is already present at _audioDataOffset
 * This function builds headers at the start and footer at the end
 * Returns the total size of the multipart body
 */
size_t ESP32AI::prepareMultipartBuffer(const String& boundary) {
    size_t position = 0;
    
    // Helper lambda to write string to buffer
    auto writeString = [this, &position](const String& str) {
        for (size_t i = 0; i < str.length(); i++) {
            _audioBuffer[position++] = (uint8_t)str[i];
        }
    };
    
    // Helper lambda to write C string to buffer
    auto writeCString = [this, &position](const char* str) {
        while (*str) {
            _audioBuffer[position++] = (uint8_t)*str++;
        }
    };
    
    // Build DeviceID field
    writeString("--");
    writeString(boundary);
    writeCString("\r\n");
    writeCString("Content-Disposition: form-data; name=\"deviceId\"\r\n\r\n");
    writeString(_deviceID);
    writeCString("\r\n");
    
    // Build Skills JSON field  
    writeString("--");
    writeString(boundary);
    writeCString("\r\n");
    writeCString("Content-Disposition: form-data; name=\"skills\"\r\n");
    writeCString("Content-Type: application/json\r\n\r\n");
    writeString(_skillsJson);
    writeCString("\r\n");
    
    // Build Audio field header
    writeString("--");
    writeString(boundary);
    writeCString("\r\n");
    writeCString("Content-Disposition: form-data; name=\"audio\"; filename=\"audio.wav\"\r\n");
    writeCString("Content-Type: audio/wav\r\n\r\n");
    
    // Insert WAV header (44 bytes) just before the audio data
    size_t audioDataSize = _audioBufferSize - _audioDataOffset;
    uint8_t wavHeader[44];
    createWavHeader(wavHeader, audioDataSize, DEFAULT_SAMPLE_RATE, DEFAULT_BITS_PER_SAMPLE, DEFAULT_CHANNELS);
    
    for (size_t i = 0; i < 44; i++) {
        _audioBuffer[position++] = wavHeader[i];
    }
    
    // Verify we didn't overwrite audio data
    if (position > _audioDataOffset) {
        Serial.printf("[ESP32AI] ERROR: Header overflow! Position: %d, AudioOffset: %d\n", 
                      position, _audioDataOffset);
        Serial.println("[ESP32AI] Header space calculation was incorrect!");
        return 0;
    }
    
    // Move position to end of audio data
    position = _audioBufferSize;
    
    // Add closing boundary (footer)
    writeCString("\r\n--");
    writeString(boundary);
    writeCString("--\r\n");
    
    // Verify we didn't exceed buffer size
    if (position > _maxBufferSize) {
        Serial.printf("[ESP32AI] ERROR: Buffer overflow! Position: %d, MaxBuffer: %d\n", 
                      position, _maxBufferSize);
        Serial.println("[ESP32AI] Footer space was insufficient!");
        return 0;
    }
    
    return position;  // Total size of multipart body
}


/**
 * Create WAV file header
 * Converts raw PCM data to WAV format for compatibility with Whisper
 */
void ESP32AI::createWavHeader(uint8_t* header, uint32_t dataSize, uint32_t sampleRate, 
                              uint16_t bitsPerSample, uint16_t channels) {
    uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    uint16_t blockAlign = channels * (bitsPerSample / 8);
    uint32_t chunkSize = 36 + dataSize;
    
    // RIFF header
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = chunkSize & 0xFF;
    header[5] = (chunkSize >> 8) & 0xFF;
    header[6] = (chunkSize >> 16) & 0xFF;
    header[7] = (chunkSize >> 24) & 0xFF;
    
    // WAVE format
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    
    // fmt subchunk
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;  // Subchunk1Size (16 for PCM)
    header[20] = 1; header[21] = 0;  // AudioFormat (1 for PCM)
    header[22] = channels & 0xFF; header[23] = (channels >> 8) & 0xFF;
    header[24] = sampleRate & 0xFF;
    header[25] = (sampleRate >> 8) & 0xFF;
    header[26] = (sampleRate >> 16) & 0xFF;
    header[27] = (sampleRate >> 24) & 0xFF;
    header[28] = byteRate & 0xFF;
    header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF;
    header[31] = (byteRate >> 24) & 0xFF;
    header[32] = blockAlign & 0xFF;
    header[33] = (blockAlign >> 8) & 0xFF;
    header[34] = bitsPerSample & 0xFF;
    header[35] = (bitsPerSample >> 8) & 0xFF;
    
    // data subchunk
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = dataSize & 0xFF;
    header[41] = (dataSize >> 8) & 0xFF;
    header[42] = (dataSize >> 16) & 0xFF;
    header[43] = (dataSize >> 24) & 0xFF;
}

/**
 * Parse WorkerAI JSON response
 */
SkillResponse ESP32AI::parseResponse(const String& jsonResponse) {
    SkillResponse response;
    
    // Validate response is not empty
    if (jsonResponse.length() == 0) {
        Serial.println("[ESP32AI] Error: Empty response from server");
        return response;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonResponse);
    
    if (error) {
        Serial.print("[ESP32AI] Error parsing response JSON: ");
        Serial.println(error.c_str());
        Serial.print("[ESP32AI] Response was: ");
        Serial.println(jsonResponse.substring(0, 100)); // Print first 100 chars for debugging
        return response;
    }
    
    // Validate response is an array
    if (!doc.is<JsonArray>()) {
        Serial.println("[ESP32AI] Error: Response is not a JSON array");
        Serial.print("[ESP32AI] Response was: ");
        Serial.println(jsonResponse.substring(0, 100));
        return response;
    }
    
    // Parse response structure (expects array of functions with actions)
    JsonArray functions = doc.as<JsonArray>();
    
    // Check if array is empty
    if (functions.size() == 0) {
        Serial.println("[ESP32AI] Warning: Empty response array from server");
        return response;
    }
    
    for (JsonObject func : functions) {
        // Validate object has required fields
        if (!func.containsKey("Target") || !func.containsKey("Action")) {
            Serial.println("[ESP32AI] Warning: Response object missing Target or Action field");
            continue;
        }
        
        String targetName = func["Target"] | "";
        String actionName = func["Action"] | "";
        
        // Validate field values
        if (targetName.length() == 0) {
            Serial.println("[ESP32AI] Warning: Empty Target name in response");
            continue;
        }
        
        // Check if this function has a non-empty action
        if (actionName.length() > 0 && 
            actionName != "None" && 
            actionName != "null" && 
            actionName != "NULL") {
            
            response.hasAction = true;
            response.targetName = targetName;
            response.actionName = actionName;
            
            // Check for action value (for integer types, etc.)
            if (func.containsKey("Value")) {
                response.actionValue = func["Value"].as<String>();
            }
            
            Serial.printf("[ESP32AI] Action detected: %s -> %s", 
                         targetName.c_str(), actionName.c_str());
            if (response.actionValue.length() > 0) {
                Serial.printf(" = %s", response.actionValue.c_str());
            }
            Serial.println();
            
            break; // Only one action per request
        }
    }
    
    if (!response.hasAction) {
        Serial.println("[ESP32AI] No action detected in response");
    }
    
    return response;
}

/**
 * Calculate 95th percentile of an array of values (for silence detection and compression)
 * Uses in-place quickselect algorithm - O(n) average time, no malloc needed
 * WARNING: This function modifies the input array (partial sorting)
 */
int32_t ESP32AI::calculate95thPercentile(int32_t* values, size_t count) {
    if (count == 0) {
        return 0;
    }
    
    if (count == 1) {
        return values[0];
    }
    
    // Calculate target index for 95th percentile
    size_t targetIndex = (size_t)((count - 1) * 0.95);
    
    // Use quickselect to find the element at targetIndex position
    // This partitions the array so that values[targetIndex] contains the 95th percentile
    int left = 0;
    int right = count - 1;
    
    while (left < right) {
        // Partition using median-of-three for better pivot selection
        int mid = left + (right - left) / 2;
        
        // Ensure values[mid] <= values[left] <= values[right]
        if (values[mid] > values[left]) {
            int32_t temp = values[mid];
            values[mid] = values[left];
            values[left] = temp;
        }
        if (values[right] > values[left]) {
            int32_t temp = values[right];
            values[right] = values[left];
            values[left] = temp;
        }
        if (values[right] > values[mid]) {
            int32_t temp = values[right];
            values[right] = values[mid];
            values[mid] = temp;
        }
        
        // Use middle element as pivot
        int32_t pivot = values[mid];
        
        // Move pivot to end
        int32_t temp = values[mid];
        values[mid] = values[right - 1];
        values[right - 1] = temp;
        
        // Partition
        int i = left;
        int j = right - 1;
        
        while (true) {
            while (i < right && values[++i] < pivot) {}
            while (j > left && values[--j] > pivot) {}
            
            if (i >= j) {
                break;
            }
            
            // Swap
            temp = values[i];
            values[i] = values[j];
            values[j] = temp;
        }
        
        // Restore pivot
        temp = values[i];
        values[i] = values[right - 1];
        values[right - 1] = temp;
        
        // Determine which partition to continue with
        if (i == targetIndex) {
            return values[i];
        } else if (i > targetIndex) {
            right = i - 1;
        } else {
            left = i + 1;
        }
    }
    
    return values[targetIndex];
}

/**
 * Validate that a URL uses HTTPS protocol
 */
bool ESP32AI::validateHttpsUrl(const String& url) {
    if (url.length() == 0) {
        return false;
    }
    
    // Check if URL starts with https://
    if (!url.startsWith("https://")) {
        return false;
    }
    
    // Additional basic validation - ensure there's a domain after https://
    if (url.length() <= 8) {  // "https://" is 8 characters
        return false;
    }
    
    return true;
}

/**
 * Normalize endpoint URL by ensuring it ends with /api/process
 * Handles cases where user omits the api route or adds extra content after it
 */
String ESP32AI::normalizeEndpointUrl(const String& url) {
    String normalized = url;
    
    // Remove trailing slashes
    while (normalized.endsWith("/")) {
        normalized = normalized.substring(0, normalized.length() - 1);
    }
    
    // Check if /api/process is already in the URL
    int processIndex = normalized.indexOf("/api/process");
    
    if (processIndex >= 0) {
        // Found /api/process - truncate everything after it
        normalized = normalized.substring(0, processIndex + 12);  // 12 = length of "/api/process"
        Serial.println("[ESP32AI] URL already contains /api/process, normalized to: " + normalized);
    } else {
        // /api/process not found - append it
        normalized += "/api/process";
        Serial.println("[ESP32AI] Appended /api/process to URL: " + normalized);
    }
    
    return normalized;
}
