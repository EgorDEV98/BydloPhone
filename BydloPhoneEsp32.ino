#include <Arduino.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/i2s.h>

#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

// ===================== User settings =====================

// BydloPhone.Server address.
const char* SERVER_HOST = "192.168.1.10";
const uint16_t SERVER_PORT = 8080;

const char* DEVICE_ID = "esp32-wroom-01";
const char* VOICE_PROFILE = "bydlo";

// LEDs. Change to match your wiring.
const int RED_LED_PIN = 2;
const int GREEN_LED_PIN = 4;

// Button: one side to GPIO, another side to GND.
const int BUTTON_PIN = 15;

// I2S microphone, for example INMP441/SPH0645.
const i2s_port_t MIC_I2S_PORT = I2S_NUM_0;
const int MIC_BCLK_PIN = 26;
const int MIC_WS_PIN = 25;
const int MIC_DIN_PIN = 33;

// MAX98357 I2S amplifier.
const i2s_port_t SPK_I2S_PORT = I2S_NUM_1;
const int SPK_BCLK_PIN = 14;
const int SPK_LRC_PIN = 27;
const int SPK_DOUT_PIN = 22;

const uint32_t RECORD_SAMPLE_RATE = 16000;
const uint16_t RECORD_BITS_PER_SAMPLE = 16;
const uint16_t RECORD_CHANNELS = 1;
const uint32_t MAX_RECORD_SECONDS = 20;
const int32_t MIC_GAIN_SHIFT = 12;

const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
const uint32_t POLL_INTERVAL_MS = 5000;

const char* AP_SSID = "BydloPhone-Setup";
const char* AP_PASSWORD = "12345678";
IPAddress AP_IP(192, 168, 100, 1);
IPAddress AP_GATEWAY(192, 168, 100, 1);
IPAddress AP_MASK(255, 255, 255, 0);

struct WavInfo {
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint32_t dataSize = 0;
};

// ===================== Globals =====================

Preferences prefs;
WebServer webServer(80);
DNSServer dnsServer;

bool configMode = false;
bool isRecording = false;
bool isPlaying = false;
uint32_t lastPollMs = 0;

const char* RECORD_PATH = "/record.wav";

// ===================== Helpers =====================

void setStatusLed(bool red, bool green) {
  digitalWrite(RED_LED_PIN, red ? HIGH : LOW);
  digitalWrite(GREEN_LED_PIN, green ? HIGH : LOW);
}

bool buttonPressed() {
  return digitalRead(BUTTON_PIN) == LOW;
}

String serverBaseUrl() {
  return String("http://") + SERVER_HOST + ":" + SERVER_PORT;
}

String htmlPage(const String& message = "") {
  String page;
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>BydloPhone WiFi</title>");
  page += F("<style>body{font-family:sans-serif;max-width:520px;margin:32px auto;padding:0 16px}");
  page += F("input,button{width:100%;box-sizing:border-box;font-size:18px;margin:8px 0;padding:10px}");
  page += F(".msg{padding:10px;background:#eee;margin:12px 0}</style></head><body>");
  page += F("<h2>BydloPhone WiFi setup</h2>");
  if (message.length() > 0) {
    page += F("<div class='msg'>");
    page += message;
    page += F("</div>");
  }
  page += F("<form method='POST' action='/save'>");
  page += F("<input name='ssid' placeholder='WiFi SSID' required>");
  page += F("<input name='password' placeholder='WiFi password' type='password'>");
  page += F("<button type='submit'>Save and connect</button>");
  page += F("</form></body></html>");
  return page;
}

void startConfigPortal(const String& message = "") {
  configMode = true;
  setStatusLed(true, false);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_MASK);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  dnsServer.start(53, "*", AP_IP);

  webServer.on("/", HTTP_GET, [message]() {
    webServer.send(200, "text/html; charset=utf-8", htmlPage(message));
  });

  webServer.on("/save", HTTP_POST, []() {
    String ssid = webServer.arg("ssid");
    String password = webServer.arg("password");

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("password", password);
    prefs.end();

    setStatusLed(true, false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      setStatusLed(false, true);
      configMode = false;
      webServer.send(
        200,
        "text/html; charset=utf-8",
        htmlPage("Connected. IP: " + WiFi.localIP().toString() + ". You can reboot the device."));
      delay(500);
      WiFi.softAPdisconnect(true);
    } else {
      setStatusLed(true, false);
      webServer.send(
        200,
        "text/html; charset=utf-8",
        htmlPage("Could not connect. Check SSID/password and try again."));
    }
  });

  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "http://192.168.100.1/", true);
    webServer.send(302, "text/plain", "");
  });

  webServer.begin();

  Serial.println("Config portal started");
  Serial.print("AP SSID: ");
  Serial.println(AP_SSID);
  Serial.println("Open http://192.168.100.1/");
}

bool connectToSavedWiFi() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("password", "");
  prefs.end();

  if (ssid.length() == 0) {
    return false;
  }

  setStatusLed(true, false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    setStatusLed(false, true);
    Serial.print("Connected to WiFi. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Saved WiFi credentials failed");
  return false;
}

void installMicI2S() {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = RECORD_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = MIC_BCLK_PIN;
  pins.ws_io_num = MIC_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_DIN_PIN;

  i2s_driver_install(MIC_I2S_PORT, &config, 0, nullptr);
  i2s_set_pin(MIC_I2S_PORT, &pins);
  i2s_zero_dma_buffer(MIC_I2S_PORT);
}

void installSpeakerI2S(uint32_t sampleRate = 24000) {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = sampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = SPK_BCLK_PIN;
  pins.ws_io_num = SPK_LRC_PIN;
  pins.data_out_num = SPK_DOUT_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(SPK_I2S_PORT, &config, 0, nullptr);
  i2s_set_pin(SPK_I2S_PORT, &pins);
  i2s_zero_dma_buffer(SPK_I2S_PORT);
}

void writeWavHeader(File& file, uint32_t dataSize) {
  uint32_t byteRate = RECORD_SAMPLE_RATE * RECORD_CHANNELS * RECORD_BITS_PER_SAMPLE / 8;
  uint16_t blockAlign = RECORD_CHANNELS * RECORD_BITS_PER_SAMPLE / 8;
  uint32_t chunkSize = 36 + dataSize;

  file.seek(0);
  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t*)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);
  file.write((const uint8_t*)"fmt ", 4);

  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1;
  file.write((uint8_t*)&subchunk1Size, 4);
  file.write((uint8_t*)&audioFormat, 2);
  file.write((uint8_t*)&RECORD_CHANNELS, 2);
  file.write((uint8_t*)&RECORD_SAMPLE_RATE, 4);
  file.write((uint8_t*)&byteRate, 4);
  file.write((uint8_t*)&blockAlign, 2);
  file.write((uint8_t*)&RECORD_BITS_PER_SAMPLE, 2);

  file.write((const uint8_t*)"data", 4);
  file.write((uint8_t*)&dataSize, 4);
}

uint32_t recordWavUntilButtonReleased() {
  isRecording = true;
  i2s_zero_dma_buffer(SPK_I2S_PORT);

  File file = SPIFFS.open(RECORD_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("Could not open record file");
    isRecording = false;
    return 0;
  }

  writeWavHeader(file, 0);

  const uint32_t maxBytes = RECORD_SAMPLE_RATE * (RECORD_BITS_PER_SAMPLE / 8) * MAX_RECORD_SECONDS;
  uint32_t dataSize = 0;
  int32_t rawSamples[256];
  int16_t pcmSamples[256];

  Serial.println("Recording started");

  while (buttonPressed() && dataSize < maxBytes) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(
      MIC_I2S_PORT,
      rawSamples,
      sizeof(rawSamples),
      &bytesRead,
      portMAX_DELAY);

    if (err != ESP_OK || bytesRead == 0) {
      continue;
    }

    size_t sampleCount = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < sampleCount; i++) {
      int32_t sample = rawSamples[i] >> MIC_GAIN_SHIFT;
      sample = constrain(sample, -32768, 32767);
      pcmSamples[i] = (int16_t)sample;
    }

    size_t bytesToWrite = sampleCount * sizeof(int16_t);
    file.write((uint8_t*)pcmSamples, bytesToWrite);
    dataSize += bytesToWrite;
  }

  writeWavHeader(file, dataSize);
  file.close();

  Serial.print("Recording finished, bytes: ");
  Serial.println(dataSize);

  isRecording = false;
  return dataSize;
}

bool readHttpStatusLine(WiFiClient& client, int& statusCode) {
  uint32_t start = millis();
  while (!client.available() && millis() - start < 10000) {
    delay(10);
  }

  if (!client.available()) {
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0 || firstSpace + 4 > (int)statusLine.length()) {
    return false;
  }

  statusCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  return statusCode > 0;
}

bool sendRecordedWav(uint32_t dataSize) {
  if (WiFi.status() != WL_CONNECTED || dataSize == 0) {
    return false;
  }

  File file = SPIFFS.open(RECORD_PATH, FILE_READ);
  if (!file) {
    Serial.println("No record file to send");
    return false;
  }

  const String boundary = "----BydloPhoneEsp32Boundary";

  String part1;
  part1 += "--" + boundary + "\r\n";
  part1 += "Content-Disposition: form-data; name=\"deviceId\"\r\n\r\n";
  part1 += DEVICE_ID;
  part1 += "\r\n";
  part1 += "--" + boundary + "\r\n";
  part1 += "Content-Disposition: form-data; name=\"voice\"\r\n\r\n";
  part1 += VOICE_PROFILE;
  part1 += "\r\n";
  part1 += "--" + boundary + "\r\n";
  part1 += "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n";
  part1 += "Content-Type: audio/wav\r\n\r\n";

  String part2;
  part2 += "\r\n--" + boundary + "--\r\n";

  uint32_t contentLength = part1.length() + file.size() + part2.length();

  WiFiClient client;
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println("Could not connect to server");
    file.close();
    return false;
  }

  client.print("POST /api/sendVoice HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(SERVER_HOST);
  client.print(":");
  client.print(SERVER_PORT);
  client.print("\r\n");
  client.print("Content-Type: multipart/form-data; boundary=");
  client.print(boundary);
  client.print("\r\n");
  client.print("Content-Length: ");
  client.print(contentLength);
  client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(part1);

  uint8_t buffer[1024];
  while (file.available()) {
    size_t n = file.read(buffer, sizeof(buffer));
    client.write(buffer, n);
  }
  file.close();

  client.print(part2);

  int statusCode = 0;
  bool ok = readHttpStatusLine(client, statusCode);
  client.stop();

  Serial.print("sendVoice status: ");
  Serial.println(ok ? statusCode : -1);

  return ok && statusCode >= 200 && statusCode < 300;
}

bool readExact(Stream& stream, uint8_t* buffer, size_t length, uint32_t timeoutMs = 10000) {
  size_t total = 0;
  uint32_t start = millis();

  while (total < length && millis() - start < timeoutMs) {
    int available = stream.available();
    if (available <= 0) {
      delay(1);
      continue;
    }

    int n = stream.readBytes(buffer + total, min((size_t)available, length - total));
    if (n > 0) {
      total += n;
      start = millis();
    }
  }

  return total == length;
}

bool skipBytes(Stream& stream, uint32_t length) {
  uint8_t buffer[128];
  while (length > 0) {
    size_t n = min((uint32_t)sizeof(buffer), length);
    if (!readExact(stream, buffer, n)) {
      return false;
    }
    length -= n;
  }
  return true;
}

uint16_t readLe16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readLe32(const uint8_t* p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

bool readWavHeader(Stream& stream, WavInfo& info) {
  uint8_t header[12];
  if (!readExact(stream, header, sizeof(header))) {
    return false;
  }

  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool gotFmt = false;
  bool gotData = false;

  while (!gotData) {
    uint8_t chunkHeader[8];
    if (!readExact(stream, chunkHeader, sizeof(chunkHeader))) {
      return false;
    }

    uint32_t chunkSize = readLe32(chunkHeader + 4);

    if (memcmp(chunkHeader, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (chunkSize < sizeof(fmt) || !readExact(stream, fmt, sizeof(fmt))) {
        return false;
      }

      uint16_t audioFormat = readLe16(fmt);
      info.channels = readLe16(fmt + 2);
      info.sampleRate = readLe32(fmt + 4);
      info.bitsPerSample = readLe16(fmt + 14);

      if (audioFormat != 1 || info.bitsPerSample != 16 || (info.channels != 1 && info.channels != 2)) {
        return false;
      }

      if (chunkSize > sizeof(fmt) && !skipBytes(stream, chunkSize - sizeof(fmt))) {
        return false;
      }
      gotFmt = true;
    } else if (memcmp(chunkHeader, "data", 4) == 0) {
      info.dataSize = chunkSize;
      gotData = true;
    } else {
      if (!skipBytes(stream, chunkSize)) {
        return false;
      }
    }

    if (chunkSize & 1) {
      if (!skipBytes(stream, 1)) {
        return false;
      }
    }
  }

  return gotFmt && gotData;
}

void playPcmStream(Stream& stream, const WavInfo& info) {
  i2s_set_sample_rates(SPK_I2S_PORT, info.sampleRate);

  uint32_t remaining = info.dataSize;
  uint8_t input[1024];
  int16_t stereo[1024];

  while (remaining > 0 && WiFi.status() == WL_CONNECTED) {
    size_t toRead = min((uint32_t)sizeof(input), remaining);
    toRead -= toRead % (info.channels * sizeof(int16_t));
    if (toRead == 0) {
      break;
    }

    if (!readExact(stream, input, toRead, 15000)) {
      break;
    }

    size_t bytesToWrite = 0;
    const uint8_t* outBytes = input;

    if (info.channels == 1) {
      size_t samples = toRead / sizeof(int16_t);
      int16_t* mono = (int16_t*)input;
      for (size_t i = 0; i < samples; i++) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
      }
      outBytes = (uint8_t*)stereo;
      bytesToWrite = samples * 2 * sizeof(int16_t);
    } else {
      bytesToWrite = toRead;
    }

    size_t written = 0;
    i2s_write(SPK_I2S_PORT, outBytes, bytesToWrite, &written, portMAX_DELAY);
    remaining -= toRead;
  }
}

bool pollAndPlayNext() {
  if (WiFi.status() != WL_CONNECTED || isRecording || isPlaying) {
    return false;
  }

  HTTPClient http;
  String url = serverBaseUrl() + "/api/getNext?deviceId=" + DEVICE_ID;
  http.begin(url);

  int code = http.GET();
  if (code == HTTP_CODE_NO_CONTENT) {
    http.end();
    return false;
  }

  if (code != HTTP_CODE_OK) {
    Serial.print("getNext status: ");
    Serial.println(code);
    http.end();
    return false;
  }

  isPlaying = true;
  Serial.println("Playing server answer");

  WiFiClient* stream = http.getStreamPtr();
  WavInfo info;
  bool ok = readWavHeader(*stream, info);
  if (ok) {
    playPcmStream(*stream, info);
  } else {
    Serial.println("Invalid WAV from server");
  }

  i2s_zero_dma_buffer(SPK_I2S_PORT);
  http.end();
  isPlaying = false;
  return ok;
}

// ===================== Arduino lifecycle =====================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setStatusLed(true, false);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  installMicI2S();
  installSpeakerI2S();

  if (!connectToSavedWiFi()) {
    startConfigPortal("Enter your WiFi credentials.");
  }
}

void loop() {
  if (configMode) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    setStatusLed(true, false);
    if (!connectToSavedWiFi()) {
      startConfigPortal("WiFi connection lost. Enter credentials again.");
    }
    return;
  }

  setStatusLed(false, true);

  if (!isPlaying && buttonPressed()) {
    delay(30);
    if (buttonPressed()) {
      uint32_t dataSize = recordWavUntilButtonReleased();
      if (dataSize > 0) {
        sendRecordedWav(dataSize);
      }
      delay(250);
    }
  }

  if (!isRecording && !isPlaying && millis() - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = millis();
    pollAndPlayNext();
  }
}
