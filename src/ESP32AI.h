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

#ifndef ESP32AI_H
#define ESP32AI_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// Maximum recording time in milliseconds (default 3500ms = 3.5 seconds or ~28KB at 8kHz 8-bit mono)
// Override this value by defining it before including ESP32AI.h
#ifndef MAX_RECORD_TIME_MS
#define MAX_RECORD_TIME_MS 3500
#endif

// Adaptive gain control (AGC) - Block-based compression configuration
// Override these values by defining them before #include ESP32AI.h
#ifndef COMPRESSION_BLOCK_SIZE
#define COMPRESSION_BLOCK_SIZE 256  // Process audio in 256 sample blocks (32ms @ 8kHz)
#endif

#ifndef COMPRESSION_TARGET_LEVEL
#define COMPRESSION_TARGET_LEVEL 100  // 78% of 128 max
#endif

// If calibration has not been performed, use this default silence threshold
#define DEFAULT_SILENCE_THRESHOLD 5000  // 85th percentile below this = silent block (24-bit scale)

// Default I2S configuration
#define DEFAULT_SAMPLE_RATE 8000
#define DEFAULT_BITS_PER_SAMPLE 8
#define DEFAULT_CHANNELS 1
#define I2S_DMA_BUF_COUNT 8
#define I2S_DMA_BUF_LEN 1024

/**
 * Structure to hold the parsed response from WorkerAI
 */
struct SkillResponse {
    bool hasAction;           // true if an action was triggered
    String targetName;        // Name of the target (e.g., "Thermostat")
    String actionName;        // Name of the action (e.g., "Set")
    String actionValue;       // Value associated with the action (if any)
    
    SkillResponse() : hasAction(false), targetName(""), actionName(""), actionValue("") {}
};

/**
 * Main ESP32AI class
 */
class ESP32AI {
public:
    /**
     * Constructor
     * @param deviceID Device identifier (optional, will load from NVS if null)
     * @param authorization Authorization token (optional, will load from NVS if null)
     * @param workerAIEndpoint WorkerAI endpoint URL (optional, will load from NVS if null)
     */
    ESP32AI(const char* deviceID = nullptr, const char* authorization = nullptr, const char* workerAIEndpoint = nullptr);
    
    /**
     * Destructor
     */
    ~ESP32AI();
    
    /**
     * Initialize the library (NVS, I2S)
     * @return true if initialization successful
     */
    bool begin();
    
    /**
     * Set the skills JSON configuration
     * @param skillsJson JSON string defining available skills
     * @return true if JSON is valid
     */
    bool setSkills(const char* skillsJson);
    
    /**
     * Configure I2S pins for microphone
     * @param bckPin Bit clock pin
     * @param wsPin Word select pin
     * @param dataPin Data pin
     */
    void configureI2S(int bckPin, int wsPin, int dataPin);
    
    /**
     * Configure the GPIO pin for recording trigger
     * @param pin GPIO pin number
     * @param activeLow true if pin is active low (default false)
     */
    void configureRecordingPin(int pin, bool activeLow = false);
    
    /**
     * Set the Cloudflare WorkerAI endpoint URL
     * @param url Full HTTPS URL to the WorkerAI endpoint
     */
    void setWorkerAIEndpoint(const char* url);
    
    /**
     * Start listening for voice commands (non-blocking check for GPIO trigger)
     * Checks if recording button is currently pressed. If not, returns immediately.
     * If pressed, records audio while GPIO pin is pressed.
     * @return true if audio was recorded successfully, false if button not pressed or recording failed
     */
    bool startListening();
    
    /**
     * Process the recorded command by sending to WorkerAI
     * @param timeoutMs HTTP request timeout (default 12000ms)
     * @return SkillResponse structure with parsed action
     */
    SkillResponse processCommand(uint32_t timeoutMs = 12000);
    
    /**
     * Get the DeviceID currently in use
     * @return DeviceID string
     */
    String getDeviceID() const { return _deviceID; }
    
    /**
     * Get the WorkerAI endpoint URL currently in use
     * @return Endpoint URL string
     */
    String getWorkerAIEndpoint() const { return _workerAIEndpoint; }
    
    /**
     * Check if authorization token is configured
     * @return true if authorization token exists
     */
    bool hasAuthorization() const { return _authorization.length() > 0; }
    
    /**
     * Set the DeviceID and save to NVS
     * @param deviceID New device ID
     * @return true if saved successfully
     */
    bool setDeviceID(const char* deviceID);
    
    /**
     * Set the Authorization token and save to NVS
     * @param authorization New authorization token
     * @return true if saved successfully
     */
    bool setAuthorization(const char* authorization);
    
    /**
     * Check if the library is properly initialized
     * @return true if ready to use
     */
    bool isReady() const { return _initialized; }
    
    /**
     * Calibrates the silence threshold by measuring ambient noise
     * Result stored in NVS for persistence
     * @param multiplier Multiplier applied to noise floor (default 2.0). Higher = Raises squelch threshold (more aggressive silence detection).
     * @return true if calibration successful
     */
    bool calibrateSilenceThreshold(float multiplier = 2.0);
    
    /**
     * Interactive setup routine via Serial Monitor
     * Displays current NVS configuration and prompts for new values
     * Handles all user input and saves configuration to NVS
     */
    void runSetup();

private:
    // NVS storage
    Preferences _preferences;
    
    // Configuration
    String _deviceID;
    String _authorization;
    String _workerAIEndpoint;
    String _skillsJson;
    
    // I2S Configuration
    int _i2sBckPin;
    int _i2sWsPin;
    int _i2sDataPin;
    i2s_port_t _i2sPort;
    
    // Recording configuration
    int _recordingPin;
    bool _recordingActiveLow;
    bool _buttonWasPressed;  // Track button state to detect transitions
    
    // State
    bool _initialized;
    bool _i2sConfigured;
    uint8_t* _audioBuffer;
    size_t _audioBufferSize;
    size_t _maxBufferSize;          // Maximum buffer size allocated (to avoid reallocation)
    size_t _multipartHeaderSpace;   // Space reserved at buffer start for multipart headers
    size_t _audioDataOffset;        // Offset where audio recording starts (after headers)
    int32_t* _blockBuffer24;        // Reusable buffer for block processing
    int32_t* _blockAbsValues;       // Reusable buffer for absolute values
    int32_t _silenceThreshold;      // 85th percentile threshold for silence detection (in 24-bit scale)
    
    // Private methods
    bool initializeNVS();
    bool initializeI2S();
    void deinitializeI2S();
    bool recordAudio();
    String sendToWorkerAI(uint32_t timeoutMs);
    SkillResponse parseResponse(const String& jsonResponse);
    size_t calculateHeaderSpace();
    size_t prepareMultipartBuffer(const String& boundary);
    void createWavHeader(uint8_t* header, uint32_t dataSize, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t channels);
    bool validateHttpsUrl(const String& url);
    String normalizeEndpointUrl(const String& url);
    int32_t calculate85thPercentile(int32_t* values, size_t count);
    String readSerialLine(unsigned long timeoutMs);
    void freeAudioBuffer();
};

#endif // ESP32AI_H
