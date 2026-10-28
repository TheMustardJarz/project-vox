#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <ArduinoJson.h>

// ==========================================
// 1. WiFi & Server Configuration
// ==========================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SERVER_URL    = "http://129.21.118.25:5000/process-audio";

// ==========================================
// 2. Hardware & Audio Configuration
// ==========================================
#define RECORD_BUTTON_PIN 0          // Boot button on most ESP32 boards (Active LOW)
#define I2S_PORT          I2S_NUM_0
#define I2S_SCK           14         // BCLK / Serial Clock
#define I2S_WS            15         // LRCLK / Word Select
#define I2S_SD            32         // DOUT / Serial Data from Mic

#define SAMPLE_RATE       16000      // 16 kHz is ideal for Whisper
#define RECORD_SECONDS    5          // Duration of recording per press
#define BUFFER_SIZE       (SAMPLE_RATE * 2 * RECORD_SECONDS) // 16-bit mono = 2 bytes/sample

uint8_t* pcm_buffer = NULL;

// ==========================================
// 3. I2S Initialization (e.g. INMP441)
// ==========================================
void init_i2s() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 1024,
        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
}

// ==========================================
// 4. Create standard 44-byte WAV header
// ==========================================
void create_wav_header(uint8_t* header, int total_data_len) {
    int total_file_len = total_data_len + 36;
    int byte_rate = SAMPLE_RATE * 1 * 2; // SampleRate * Channels * BitsPerSample/8

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)(total_file_len & 0xff);
    header[5] = (uint8_t)((total_file_len >> 8) & 0xff);
    header[6] = (uint8_t)((total_file_len >> 16) & 0xff);
    header[7] = (uint8_t)((total_file_len >> 24) & 0xff);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; // Subchunk1Size (16 for PCM)
    header[20] = 1; header[21] = 0;                                   // AudioFormat (1 = PCM)
    header[22] = 1; header[23] = 0;                                   // NumChannels (1 = Mono)
    header[24] = (uint8_t)(SAMPLE_RATE & 0xff);
    header[25] = (uint8_t)((SAMPLE_RATE >> 8) & 0xff);
    header[26] = (uint8_t)((SAMPLE_RATE >> 16) & 0xff);
    header[27] = (uint8_t)((SAMPLE_RATE >> 24) & 0xff);
    header[28] = (uint8_t)(byte_rate & 0xff);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xff);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    header[32] = 2; header[33] = 0;                                   // BlockAlign (Channels * BitsPerSample/8)
    header[34] = 16; header[35] = 0;                                  // BitsPerSample (16)
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(total_data_len & 0xff);
    header[41] = (uint8_t)((total_data_len >> 8) & 0xff);
    header[42] = (uint8_t)((total_data_len >> 16) & 0xff);
    header[43] = (uint8_t)((total_data_len >> 24) & 0xff);
}

// ==========================================
// 5. Send Audio via HTTP POST
// ==========================================
void send_audio_to_server() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] WiFi not connected!");
        return;
    }

    Serial.println("[HTTP] Connecting to server...");
    HTTPClient http;
    http.begin(SERVER_URL);
    http.setTimeout(30000); // 30 sec timeout for Whisper + LLM processing

    // Multipart boundary definition
    const char* boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";
    char header_str[128];
    snprintf(header_str, sizeof(header_str), "multipart/form-data; boundary=%s", boundary);
    http.addHeader("Content-Type", header_str);

    // Build multipart preamble and epilogue
    String body_preamble = String("--") + boundary + "\r\n" +
                           "Content-Disposition: form-data; name=\"audio\"; filename=\"esp32_audio.wav\"\r\n" +
                           "Content-Type: audio/wav\r\n\r\n";
    String body_epilogue = String("\r\n--") + boundary + "--\r\n";

    uint8_t wav_header[44];
    create_wav_header(wav_header, BUFFER_SIZE);

    int total_payload_size = body_preamble.length() + sizeof(wav_header) + BUFFER_SIZE + body_epilogue.length();

    // Allocate continuous buffer for the full HTTP POST body (or send via WiFiClient stream)
    WiFiClient* client = http.getStreamPtr();
    
    // We send request manually over the raw TCP stream for memory efficiency
    int http_response_code = http.sendRequest("POST", (uint8_t*)NULL, 0); // Open connection with headers
    // For simpler memory allocation on ESP32, use WiFiClient directly:
    http.end(); // close temporary handle, we do raw WiFiClient stream below
    
    WiFiClient tcp_client;
    if (!tcp_client.connect("129.21.118.25", 5000)) {
        Serial.println("[HTTP] Connection to 129.21.118.25:5000 failed!");
        return;
    }

    // Write HTTP headers
    tcp_client.print("POST /process-audio HTTP/1.1\r\n");
    tcp_client.print("Host: 129.21.118.25:5000\r\n");
    tcp_client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    tcp_client.printf("Content-Length: %d\r\n", total_payload_size);
    tcp_client.print("Connection: close\r\n\r\n");

    // Write body parts:
    // 1) Preamble
    tcp_client.print(body_preamble);
    // 2) WAV Header (44 bytes)
    tcp_client.write(wav_header, sizeof(wav_header));
    // 3) Raw PCM samples in chunks
    size_t chunk_size = 1024;
    for (size_t offset = 0; offset < BUFFER_SIZE; offset += chunk_size) {
        size_t to_write = min(chunk_size, (size_t)(BUFFER_SIZE - offset));
        tcp_client.write(pcm_buffer + offset, to_write);
    }
    // 4) Epilogue
    tcp_client.print(body_epilogue);

    Serial.println("[HTTP] Audio streamed. Waiting for server response...");

    // Read response
    while (tcp_client.connected() || tcp_client.available()) {
        if (tcp_client.available()) {
            String line = tcp_client.readStringUntil('\n');
            if (line == "\r") {
                // Headers ended, read JSON body next
                break;
            }
        }
        delay(10);
    }

    String json_body = tcp_client.readString();
    tcp_client.stop();

    Serial.println("\n[SERVER RESPONSE]:");
    Serial.println(json_body);

    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json_body);
    if (!error) {
        const char* prompt = doc["prompt"];
        const char* response = doc["response"];
        Serial.println("==================================");
        Serial.printf("Prompt:   %s\n", prompt);
        Serial.printf("Response: %s\n", response);
        Serial.println("==================================");
    } else {
        Serial.println("JSON parse error or non-JSON response.");
    }
}

// ==========================================
// 6. Record Audio from I2S
// ==========================================
void record_audio() {
    Serial.println("Recording started... Speak into the microphone.");
    size_t bytes_read = 0;
    size_t total_bytes_read = 0;

    while (total_bytes_read < BUFFER_SIZE) {
        size_t to_read = min((size_t)1024, (size_t)(BUFFER_SIZE - total_bytes_read));
        esp_err_t res = i2s_read(I2S_PORT, (void*)(pcm_buffer + total_bytes_read), to_read, &bytes_read, portMAX_DELAY);
        if (res == ESP_OK) {
            total_bytes_read += bytes_read;
        }
    }
    Serial.println("Recording finished!");
}

void setup() {
    Serial.begin(115200);
    pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);

    // Allocate PSRAM or Heap buffer for audio (160 KB for 5 sec @ 16kHz 16-bit mono)
    if (psramFound()) {
        pcm_buffer = (uint8_t*)ps_malloc(BUFFER_SIZE);
        Serial.println("Allocated audio buffer in PSRAM");
    } else {
        pcm_buffer = (uint8_t*)malloc(BUFFER_SIZE);
        Serial.println("Allocated audio buffer in internal SRAM");
    }

    if (!pcm_buffer) {
        Serial.println("Failed to allocate audio buffer!");
        while (1) delay(1000);
    }

    // Connect to WiFi
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());

    // Initialize I2S microphone
    init_i2s();
    Serial.println("System ready. Press the BOOT button (GPIO 0) to record.");
}

void loop() {
    // Detect button press (Active LOW)
    if (digitalRead(RECORD_BUTTON_PIN) == LOW) {
        delay(50); // Debounce
        if (digitalRead(RECORD_BUTTON_PIN) == LOW) {
            record_audio();
            send_audio_to_server();
            Serial.println("Ready for next recording. Press button to record again.");
            delay(1000);
        }
    }
    delay(100);
}
