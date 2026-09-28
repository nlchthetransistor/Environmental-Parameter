// 1. Includes
#include <Wire.h>
#include <DHT.h>
#include <MS5611.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <DFRobot_B_LUX_V30B.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <time.h>

// 2. ============ CẤU HÌNH — DEV CHỈNH SỬA TRƯỚC KHI NẠP ============
#define WIFI_SSID     "Transistor"       // Đổi thành WiFi tại hiện trường
#define WIFI_PASS     "hieuvjppro218"    // Đổi thành mật khẩu WiFi
#define MQTT_BROKER   "45.117.179.192"   // IP/domain MQTT broker
#define MQTT_PORT     1883
#define MQTT_TOPIC    "sensor/data"      // Topic gửi dữ liệu
#define CLUSTER_CODE  "C003"             // Mã cụm nhà kính
#define CLIENT_ID     "C003_NhaKinh"     // MQTT Client ID — KHÔNG ĐƯỢC TRÙNG

#define FW_VERSION "1.0.0"
#define FOTA_CHECK_INTERVAL 21600000UL     // 6 giờ (ms)

#define DHTPIN 10       // Chân IO10
#define DHTTYPE DHT12
#define LEDPIN 40

#define WDT_TIMEOUT 30  // Watchdog timeout 30 giây
#define SENSOR_UPDATE_INTERVAL 2500 // Chu kỳ đọc và gửi cảm biến

// 3. Global variables
DHT dht(DHTPIN, DHTTYPE);
MS5611 ms5611(0x77);  // Địa chỉ I2C MS5611
TwoWire Wire2 = TwoWire(2);
DFRobot_B_LUX_V30B luxSensor(0, 17, 18); // cEN, SCL, SDA

WiFiClient espClient;
PubSubClient mqttClient(espClient);

bool ms5611Available = false;

// Biến lưu giá trị đọc được
String temperature = "--";
String humidity = "--";
String ms5611Pressure = "--";
String luxValue = "--";

unsigned long lastSensorUpdate = 0;
unsigned long lastMqttReconnect = 0;
unsigned long lastFotaCheck = 0;
unsigned long ledTurnOnTime = 0;
bool isLedOn = false;

// Cấu trúc và bộ đệm Ring Buffer (Offline data)
struct BufferData {
  String payload;
  unsigned long timestamp;
};

#define BUFFER_SIZE 100
BufferData dataBuffer[BUFFER_SIZE];
int bufferHead = 0;
int bufferTail = 0;
int bufferCount = 0;

// Function Prototypes
void setupNTP();
unsigned long getEpochTime();
void performFOTA(String url);

// 4. Sensor functions
// Hàm đọc nhiệt độ theo ưu tiên MS5611, fallback DHT12
String readTemperature() {
  if (ms5611Available) {
    if (ms5611.read()) {   // đọc thành công
      float temp = ms5611.getTemperature();
      if (!isnan(temp)) {
        return String(temp, 2);
      }
    }
  }
  float t = dht.readTemperature();
  return isnan(t) ? "--" : String(t, 2);
}

// Hàm đọc áp suất từ MS5611
String readPressure() {
  if (ms5611Available) {
    if (ms5611.read()) {
      float pressure_mbar = ms5611.getPressure();
      Serial.print("Raw pressure: ");
      Serial.println(pressure_mbar); // Xem giá trị raw thực tế
      if (!isnan(pressure_mbar)) {
        float pressure_atm = pressure_mbar * 0.0009869233;
        return String(pressure_atm, 5);
      }
    } else {
      Serial.println("ms5611.read() failed.");
    }
  }
  return "--"; // Fix lỗi MS5611 không có giá trị
}

// Đọc độ ẩm bằng DHT12
String readDHTHumidity() {
  float h = dht.readHumidity();
  return isnan(h) ? "--" : String(h, 2);
}

// Đọc ánh sáng từ SEN0390
String readLux() {
  float lux = luxSensor.lightStrengthLux();
  if (lux < 0) return "--";
  return String(int(lux));
}

// Cập nhật tất cả các cảm biến
void updateAllSensors() {
  temperature = readTemperature();
  humidity = readDHTHumidity();
  ms5611Pressure = readPressure();
  luxValue = readLux();

  Serial.println("Cập nhật cảm biến:");
  Serial.print("Nhiệt độ: ");
  Serial.print(temperature);
  Serial.print(" °C\t");

  Serial.print("Độ ẩm: ");
  Serial.print(humidity);
  Serial.print(" %\t");

  Serial.print("Áp suất: ");
  Serial.print(ms5611Pressure);
  Serial.println(" atm");

  Serial.print("Lux: ");
  Serial.print(luxValue);
  Serial.println(" lx");

  Serial.println("-----------------------------");
}

// 5. WiFi functions
void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi mất kết nối. Đang kết nối lại...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    // Non-blocking WiFi connect
  }
}

// 8. Buffer functions
void pushToBuffer(String payload) {
  unsigned long ts = getEpochTime();
  dataBuffer[bufferHead].payload = payload;
  dataBuffer[bufferHead].timestamp = ts;
  bufferHead = (bufferHead + 1) % BUFFER_SIZE;
  
  if (bufferCount < BUFFER_SIZE) {
    bufferCount++;
  } else {
    // Đầy buffer, ghi đè phần tử cũ nhất
    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
  }
}

void flushBuffer() {
  while (bufferCount > 0) {
    if (!mqttClient.connected()) break; // Nếu mất kết nối trong lúc flush thì dừng
    String p = dataBuffer[bufferTail].payload;
    mqttClient.publish(MQTT_TOPIC, p.c_str());
    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
    bufferCount--;
    delay(10); // Ngăn việc spam broker quá mức (nhưng không chiếm nhiều tài nguyên)
    esp_task_wdt_reset();
  }
}

// 6. MQTT functions
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.print("MQTT Received [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(msg);

  String fotaTopic = String("fota/") + CLUSTER_CODE;
  if (String(topic) == fotaTopic) {
    Serial.println("Kích hoạt FOTA qua MQTT...");
    performFOTA(msg);
  }
}

void checkMQTT() {
  if (!mqttClient.connected()) {
    if (millis() - lastMqttReconnect > 5000) { // Thử lại sau mỗi 5s
      lastMqttReconnect = millis();
      Serial.print("Đang kết nối MQTT...");
      
      if (mqttClient.connect(CLIENT_ID)) {
        Serial.println("Đã kết nối!");
        
        // Subscribe topic FOTA trigger
        String fotaTopic = String("fota/") + CLUSTER_CODE;
        mqttClient.subscribe(fotaTopic.c_str());
        
        // Gửi toàn bộ data offline trong buffer
        flushBuffer();
      } else {
        Serial.print("Thất bại, lỗi=");
        Serial.println(mqttClient.state());
      }
    }
  }
}

// 7. FOTA functions
void performFOTA(String url) {
  WiFiClientSecure clientSecure;
  clientSecure.setInsecure(); // Bỏ qua kiểm tra chứng chỉ SSL
  
  String statusTopic = "fota/status";
  String responsePayload = String(CLUSTER_CODE) + "_OTA_";
  
  Serial.println("Bắt đầu FOTA từ URL: " + url);
  t_httpUpdate_return ret = httpUpdate.update(clientSecure, url);
  
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("FOTA Thất bại (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      responsePayload += "FAIL_" + httpUpdate.getLastErrorString();
      mqttClient.publish(statusTopic.c_str(), responsePayload.c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("FOTA Không có bản cập nhật");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("FOTA Thành công!");
      responsePayload += "OK_" + String(FW_VERSION);
      mqttClient.publish(statusTopic.c_str(), responsePayload.c_str());
      delay(1000);
      ESP.restart(); // Khởi động lại sau khi update thành công
      break;
  }
}

void checkFOTAGitHub() {
  if (millis() - lastFotaCheck > FOTA_CHECK_INTERVAL) {
    lastFotaCheck = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Đang kiểm tra cập nhật FOTA trên GitHub...");
      WiFiClientSecure clientSecure;
      clientSecure.setInsecure();
      HTTPClient http;
      
      http.begin(clientSecure, "https://api.github.com/repos/nlchthetransistor/Environmental-Parameter/releases/latest");
      int httpCode = http.GET();
      
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error) {
          String latestVersion = doc["tag_name"].as<String>();
          if (latestVersion != "" && latestVersion != FW_VERSION) {
            Serial.println("Phát hiện phiên bản mới: " + latestVersion);
            String downloadUrl = doc["assets"][0]["browser_download_url"].as<String>();
            if (downloadUrl != "") {
              performFOTA(downloadUrl);
            }
          } else {
            Serial.println("Firmware đã ở phiên bản mới nhất.");
          }
        }
      } else {
        Serial.printf("Lỗi khi kết nối GitHub API: %d\n", httpCode);
      }
      http.end();
    }
  }
}

// 9. NTP functions
void setupNTP() {
  configTime(7 * 3600, 0, "pool.ntp.org");
  Serial.print("Đang đồng bộ NTP...");
  time_t now = time(nullptr);
  int retry = 0;
  while (now < 24 * 3600 && retry < 10) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }
  Serial.println("\nĐã đồng bộ NTP!");
}

unsigned long getEpochTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return 0; // Trả về 0 nếu chưa đồng bộ
  }
  time(&now);
  return now;
}

// 10. setup()
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Khởi tạo Watchdog Timer (30 giây)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  pinMode(LEDPIN, OUTPUT);
  digitalWrite(LEDPIN, LOW);

  // Khởi tạo WiFi
  Serial.print("Kết nối WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
  }
  Serial.println("\nĐã kết nối WiFi, IP: ");
  Serial.println(WiFi.localIP());

  setupNTP();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  // Khởi tạo các chân I2C và cảm biến
  Wire.begin(12, 11);  // GY63 MS5611
  Wire2.begin(18, 17); // Light Sensor
  dht.begin();

  Serial.println("Khởi động cảm biến MS5611...");
  if (ms5611.begin() == true) {
    ms5611Available = true;
    Serial.println("MS5611 đã kết nối thành công!");
  } else {
    ms5611Available = false;
    Serial.println("Không tìm thấy MS5611, sử dụng DHT12 để đọc nhiệt độ và độ ẩm.");
  }
  
  Serial.println("Khởi động cảm biến ánh sáng SEN0390...");
  luxSensor.begin();
  
  // Đặt bộ đếm để cập nhật dữ liệu ngay
  lastSensorUpdate = millis() - SENSOR_UPDATE_INTERVAL;
}

// 11. loop()
void loop() {
  esp_task_wdt_reset(); // Reset Watchdog liên tục
  
  checkWiFi();
  
  if (WiFi.status() == WL_CONNECTED) {
    checkMQTT();
    mqttClient.loop();
    checkFOTAGitHub();
  }

  // Quản lý LED chỉ báo (Non-blocking)
  if (isLedOn && (millis() - ledTurnOnTime >= 200)) {
    digitalWrite(LEDPIN, LOW);
    isLedOn = false;
  }

  // Đọc và gửi dữ liệu cảm biến định kỳ
  if (millis() - lastSensorUpdate >= SENSOR_UPDATE_INTERVAL) {
    lastSensorUpdate = millis();
    updateAllSensors();

    String payloads[4] = {
      String(CLUSTER_CODE) + "_T_" + temperature,
      String(CLUSTER_CODE) + "_H_" + humidity,
      String(CLUSTER_CODE) + "_P_" + ms5611Pressure,
      String(CLUSTER_CODE) + "_L_" + luxValue
    };

    bool connected = mqttClient.connected();
    for (int i = 0; i < 4; i++) {
      if (connected) {
        mqttClient.publish(MQTT_TOPIC, payloads[i].c_str());
        Serial.print("Sent MQTT: ");
        Serial.println(payloads[i]);
      } else {
        pushToBuffer(payloads[i]);
        Serial.print("Lưu vào buffer (Mất kết nối MQTT): ");
        Serial.println(payloads[i]);
      }
    }
    
    // Bật LED chỉ báo 200ms
    digitalWrite(LEDPIN, HIGH);
    isLedOn = true;
    ledTurnOnTime = millis();
  }
}
