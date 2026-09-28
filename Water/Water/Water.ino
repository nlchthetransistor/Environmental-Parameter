// 1. Includes
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_task_wdt.h>

// 2. Config Section
// ============ CẤU HÌNH — DEV CHỈNH SỬA TRƯỚC KHI NẠP ============
#define WIFI_SSID     "Transistor"       // Đổi thành WiFi tại hiện trường
#define WIFI_PASS     "hieuvjppro218"    // Đổi thành mật khẩu WiFi
#define MQTT_BROKER   "45.117.179.192"   // IP/domain MQTT broker
#define MQTT_PORT     1883
#define MQTT_TOPIC    "sensor/data"      // Topic gửi dữ liệu
#define CLUSTER_CODE  "C004"             // Mã cụm nước
#define CLIENT_ID     "C004_Water"       // MQTT Client ID — KHÔNG ĐƯỢC TRÙNG

#define FW_VERSION "1.0.0"               // Phiên bản Firmware hiện tại
#define FOTA_CHECK_INTERVAL 21600000UL   // Kiểm tra bản cập nhật mỗi 6 giờ (ms)
#define FOTA_TRIGGER_TOPIC "fota/C004"
#define FOTA_STATUS_TOPIC "fota/status"

#define UART_TX_PIN 3
#define UART_RX_PIN 9
#define RS485_RE_DE_PIN 8
#define LED_GPIO    40

#define WDT_TIMEOUT 30                   // Task WDT 30s
#define SENSOR_READ_INTERVAL 5000UL      // Đọc cảm biến mỗi 5s

// 3. Global variables
WiFiClient espClient;
WiFiClientSecure httpsClient;
PubSubClient mqttClient(espClient);
ModbusMaster node;

unsigned long lastWifiCheck = 0;
unsigned long lastMqttCheck = 0;
unsigned long lastFotaCheck = 0;
unsigned long lastSensorRead = 0;
unsigned long ledTurnOnTime = 0;
bool isLedOn = false;

// Cấu trúc Ring Buffer cho dữ liệu offline
struct BufferEntry {
  char payload[128];
  unsigned long timestamp;
};

#define BUFFER_SIZE 100
BufferEntry dataBuffer[BUFFER_SIZE];
int bufferHead = 0;
int bufferTail = 0;
int bufferCount = 0;

// 4. Modbus/Sensor functions
void preTransmission() {
  digitalWrite(RS485_RE_DE_PIN, 1);
}

void postTransmission() {
  digitalWrite(RS485_RE_DE_PIN, 0);
}

int16_t readModbusInt16(uint8_t slave_addr, uint16_t reg_addr) {
  node.begin(slave_addr, Serial1);
  uint8_t result = node.readHoldingRegisters(reg_addr, 1);
  if (result == node.ku8MBSuccess) {
    return node.getResponseBuffer(0);
  } else {
    Serial.printf("Modbus read fail slave=0x%02X reg=0x%04X err=%d\n", slave_addr, reg_addr, result);
    return 0;
  }
}

// 8. Buffer functions
void pushBuffer(const char* payload) {
  time_t now;
  time(&now);
  strncpy(dataBuffer[bufferHead].payload, payload, sizeof(dataBuffer[bufferHead].payload));
  dataBuffer[bufferHead].timestamp = now;

  bufferHead = (bufferHead + 1) % BUFFER_SIZE;
  if (bufferCount < BUFFER_SIZE) {
    bufferCount++;
  } else {
    bufferTail = (bufferTail + 1) % BUFFER_SIZE; // Ghi đè phần tử cũ nhất
  }
}

void flushBuffer() {
  while (bufferCount > 0 && mqttClient.connected()) {
    char fullPayload[256];
    // Gửi kèm timestamp (ví dụ: data|timestamp) để xử lý trên server nếu cần
    snprintf(fullPayload, sizeof(fullPayload), "%s|%lu", dataBuffer[bufferTail].payload, dataBuffer[bufferTail].timestamp);
    mqttClient.publish(MQTT_TOPIC, fullPayload);
    
    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
    bufferCount--;
    delay(10); // Tránh quá tải buffer của MQTT Client
  }
}

// 7. FOTA functions
void performOTA(const char* bin_url) {
  Serial.printf("Bắt đầu FOTA từ URL: %s\n", bin_url);
  httpsClient.setInsecure();
  
  t_httpUpdate_return ret = httpUpdate.update(httpsClient, bin_url);
  
  String statusMsg;
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      statusMsg = String(CLUSTER_CODE) + "_OTA_FAIL_" + httpUpdate.getLastErrorString();
      Serial.println(statusMsg);
      if (mqttClient.connected()) mqttClient.publish(FOTA_STATUS_TOPIC, statusMsg.c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("FOTA: Không có bản cập nhật mới");
      break;
    case HTTP_UPDATE_OK:
      statusMsg = String(CLUSTER_CODE) + "_OTA_OK";
      Serial.println(statusMsg);
      if (mqttClient.connected()) mqttClient.publish(FOTA_STATUS_TOPIC, statusMsg.c_str());
      delay(1000); // Cho MQTT kịp gửi
      ESP.restart();
      break;
  }
}

void checkGitHubFOTA() {
  Serial.println("Đang kiểm tra bản cập nhật trên GitHub...");
  httpsClient.setInsecure();
  HTTPClient http;
  
  http.begin(httpsClient, "https://api.github.com/repos/nlchthetransistor/Environmental-Parameter/releases/latest");
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      const char* tag_name = doc["tag_name"]; // Ví dụ "1.0.1"
      if (tag_name != nullptr && strcmp(tag_name, FW_VERSION) != 0) {
        Serial.printf("Có bản cập nhật mới: %s (Hiện tại: %s)\n", tag_name, FW_VERSION);
        
        JsonArray assets = doc["assets"];
        for (JsonObject asset : assets) {
          const char* download_url = asset["browser_download_url"];
          const char* asset_name = asset["name"];
          // Tìm file .bin
          if (download_url != nullptr && asset_name != nullptr && String(asset_name).endsWith(".bin")) {
            performOTA(download_url);
            break;
          }
        }
      } else {
        Serial.println("Đang dùng phiên bản mới nhất.");
      }
    } else {
      Serial.println("Lỗi phân tích JSON từ GitHub API");
    }
  } else {
    Serial.printf("Lỗi kết nối GitHub API, HTTP Code: %d\n", httpCode);
  }
  http.end();
}

// 6. MQTT functions
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.printf("MQTT Nhận: %s, Data: %s\n", topic, msg.c_str());
  
  if (String(topic) == FOTA_TRIGGER_TOPIC) {
    performOTA(msg.c_str());
  }
}

void handleMQTT() {
  if (!mqttClient.connected()) {
    if (millis() - lastMqttCheck > 5000UL) {
      lastMqttCheck = millis();
      Serial.print("Đang kết nối MQTT...");
      if (mqttClient.connect(CLIENT_ID)) {
        Serial.println("Thành công");
        mqttClient.subscribe(FOTA_TRIGGER_TOPIC);
        flushBuffer(); // Xả buffer ngay khi kết nối lại
      } else {
        Serial.print("Lỗi (");
        Serial.print(mqttClient.state());
        Serial.println(")");
      }
    }
  } else {
    mqttClient.loop();
  }
}

// 5. WiFi functions
void handleWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiCheck > 10000UL) { // Thử kết nối lại mỗi 10s
      lastWifiCheck = millis();
      Serial.println("WiFi mất kết nối, đang thử lại...");
      WiFi.reconnect();
    }
  }
}

// 9. NTP functions
void setupNTP() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // GMT+7
  Serial.print("Đang đồng bộ NTP");
  int retry = 0;
  while (time(nullptr) < 100000 && retry < 20) { // Đợi đồng bộ
    Serial.print(".");
    delay(500);
    retry++;
  }
  if (time(nullptr) > 100000) {
    Serial.println("\nĐồng bộ NTP thành công.");
  } else {
    Serial.println("\nĐồng bộ NTP thất bại (timeout).");
  }
}

// 10. setup()
void setup() {
  pinMode(LED_GPIO, OUTPUT);
  pinMode(RS485_RE_DE_PIN, OUTPUT);
  digitalWrite(RS485_RE_DE_PIN, 0); // Chế độ nhận
  
  Serial.begin(115200);
  
  // Thiết lập WDT
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // Monitor all cores
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  
  // Khởi tạo Modbus
  Serial1.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  node.begin(1, Serial1);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  
  // Kết nối WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Đang kết nối WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi thành công.");
  
  setupNTP();
  
  // Cấu hình MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  
  // Gọi HTTPClient insecure checkFota đầu tiên
  httpsClient.setInsecure();
  
  lastFotaCheck = millis(); // Hoãn kiểm tra FOTA đầu tiên để chạy ổn định trước
}

// 11. loop()
void loop() {
  // 1. Reset WDT
  esp_task_wdt_reset();
  
  // 2. Xử lý kết nối WiFi và MQTT (Non-blocking)
  handleWiFi();
  handleMQTT();
  
  // 3. Xử lý tắt LED không block
  if (isLedOn && (millis() - ledTurnOnTime > 200)) {
    digitalWrite(LED_GPIO, LOW);
    isLedOn = false;
  }
  
  // 4. Kiểm tra FOTA định kỳ
  if (millis() - lastFotaCheck > FOTA_CHECK_INTERVAL) {
    lastFotaCheck = millis();
    if (WiFi.status() == WL_CONNECTED) {
      checkGitHubFOTA();
    }
  }
  
  // 5. Đọc và gửi dữ liệu cảm biến
  if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();
    
    // Bật LED báo hiệu
    digitalWrite(LED_GPIO, HIGH);
    ledTurnOnTime = millis();
    isLedOn = true;
    
    int16_t raw_do = readModbusInt16(0x02, 0x01); delay(50); // delay nhỏ để RS485 kịp xả
    int16_t raw_ph = readModbusInt16(0x03, 0x01); delay(50);
    int16_t raw_ec = readModbusInt16(0x01, 0x01); delay(50);
    
    int16_t t_do = readModbusInt16(0x02, 0x00); delay(50);
    int16_t t_ph = readModbusInt16(0x03, 0x00); delay(50);
    int16_t t_ec = readModbusInt16(0x01, 0x00); delay(50);
    
    float do_val = raw_do / 100.0f;
    float ph_val = raw_ph / 100.0f;
    float ec_val = raw_ec / 100.0f;
    float temp_avg = ((float)t_do + (float)t_ph + (float)t_ec) / 3.0f / 10.0f;
    
    Serial.printf("DO=%.2f pH=%.2f EC=%.2f TempAvg=%.2f\n", do_val, ph_val, ec_val, temp_avg);
    
    char payload[128];
    
    // Gửi hoặc lưu đệm
    snprintf(payload, sizeof(payload), "%s_DO_%.2f", CLUSTER_CODE, do_val);
    if (mqttClient.connected()) mqttClient.publish(MQTT_TOPIC, payload); else pushBuffer(payload);
    
    snprintf(payload, sizeof(payload), "%s_PH_%.2f", CLUSTER_CODE, ph_val);
    if (mqttClient.connected()) mqttClient.publish(MQTT_TOPIC, payload); else pushBuffer(payload);
    
    snprintf(payload, sizeof(payload), "%s_EC_%.2f", CLUSTER_CODE, ec_val);
    if (mqttClient.connected()) mqttClient.publish(MQTT_TOPIC, payload); else pushBuffer(payload);
    
    snprintf(payload, sizeof(payload), "%s_T_%.2f", CLUSTER_CODE, temp_avg);
    if (mqttClient.connected()) mqttClient.publish(MQTT_TOPIC, payload); else pushBuffer(payload);
  }
}
