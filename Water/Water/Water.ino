#include <WiFi.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>

// UART RS485 (ESP32-S3 pins) - nhớ đấu chân vật lý đúng theo mô tả!
#define UART_TX_PIN 3
#define UART_RX_PIN 9
#define LED_GPIO    40

#define WIFI_SSID   "Transistor"
#define WIFI_PASS   "hieuvjppro218"
#define MQTT_BROKER "45.117.179.192"
#define MQTT_PORT   1883
#define MQTT_TOPIC  "sensor/data"
#define CLUSTER_CODE "C004"

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Tạo đối tượng ModbusMaster, chọn Serial1 với RX, TX như trên
ModbusMaster node;

// Đặt địa chỉ hoặc port vật lý phù hợp (RS485: TX, RX, baud)
void preTransmission() {
  digitalWrite(8, 1); // RS485 DE/RE control (chân 8: điều khiển transmitter enable, nhớ đấu đúng)
}

void postTransmission() {
  digitalWrite(8, 0); // RS485 DE/RE control, chuyển sang nhận
}

void setup() {
  pinMode(LED_GPIO, OUTPUT);
  pinMode(8, OUTPUT); // DE/RE chân điều khiển cho RS485, nếu bạn đấu với chân khác đổi lại
  digitalWrite(8, 0); // Đầu tiên đặt chế độ nhận

  Serial.begin(115200);

  Serial1.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN); // RS485 bus
  node.begin(1, Serial1); // ID không dùng ở đây, phải đổi khi gọi mỗi thiết bị!
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi connected.");

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  while (!mqttClient.connected()) {
    Serial.print("Connecting MQTT...");
    if (mqttClient.connect("ESP32Client")) {
      Serial.println("OK");
    } else {
      Serial.print("Failed (");
      Serial.print(mqttClient.state());
      Serial.println("), retry...");
      delay(2000);
    }
  }
  Serial.println("MQTT ready.");
}

int16_t readModbusInt16(uint8_t slave_addr, uint16_t reg_addr) {
  node.begin(slave_addr, Serial1); // Gán Node ID cho thiết bị muốn đọc
  uint8_t result = node.readHoldingRegisters(reg_addr, 1); // Đọc 1 thanh ghi
  if (result == node.ku8MBSuccess) {
    return node.getResponseBuffer(0); // Giá trị 16-bit
  } else {
    Serial.printf("Modbus read fail slave=0x%02X reg=0x%04X err=%d\n", slave_addr, reg_addr, result);
    return 0;
  }
}

void loop() {
  if (!mqttClient.connected()) {
    while (!mqttClient.connected()) {
      mqttClient.connect("ESP32Client");
      delay(500);
    }
    Serial.println("Reconnected MQTT");
  }
  mqttClient.loop();

  int16_t raw_do = readModbusInt16(0x02, 0x01); delay(50);
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
  snprintf(payload, sizeof(payload), "%s_DO_%.2f", CLUSTER_CODE, do_val);
  mqttClient.publish(MQTT_TOPIC, payload); delay(80);

  snprintf(payload, sizeof(payload), "%s_PH_%.2f", CLUSTER_CODE, ph_val);
  mqttClient.publish(MQTT_TOPIC, payload); delay(80);

  snprintf(payload, sizeof(payload), "%s_EC_%.2f", CLUSTER_CODE, ec_val);
  mqttClient.publish(MQTT_TOPIC, payload); delay(80);

  snprintf(payload, sizeof(payload), "%s_T_%.2f", CLUSTER_CODE, temp_avg);
  mqttClient.publish(MQTT_TOPIC, payload); delay(80);

  digitalWrite(LED_GPIO, HIGH); delay(2500); digitalWrite(LED_GPIO, LOW);
  delay(2500);
}
