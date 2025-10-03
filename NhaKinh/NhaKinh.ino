#include <Wire.h>
#include <DHT.h>
#include <MS5611.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <DFRobot_B_LUX_V30B.h>

#define DHTPIN 10       // Chân IO10
#define DHTTYPE DHT12
#define LEDPIN 40


DHT dht(DHTPIN, DHTTYPE);
MS5611 ms5611(0x77);  // Địa chỉ I2C MS5611

TwoWire Wire2 = TwoWire(2);
DFRobot_B_LUX_V30B luxSensor(0, 17, 18); // cEN, SCL, SDA (theo thư viện)

// Thông tin WiFi
const char* ssid = "Transistor";
const char* password = "hieuvjppro218";

// Thông tin MQTT
const char* mqtt_server = "45.117.179.192"; 
const int mqtt_port = 1883;
const char* mqtt_user = "";     // nếu không cần thì để rỗng
const char* mqtt_pass = "";
const char* mqtt_topic = "sensor/data";  // Topic
const char* cluster_code = "C003";       // Mã cụm


WiFiClient espClient;
PubSubClient client(espClient);

bool ms5611Available = false;

// Biến lưu giá trị đọc được (chuẩn bị global để dùng trong updateAllSensors)
String temperature = "--";
String humidity = "--";
String ms5611Pressure = "--";
String luxValue = "--";

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Kết nối WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nĐã kết nối WiFi, IP: ");
  Serial.println(WiFi.localIP());
}

// Hàm kết nối MQTT broker
void reconnect() {
  while (!client.connected()) {
    Serial.print("Đang kết nối MQTT...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_pass)) {
      Serial.println("đã kết nối!");
    } else {
      Serial.print("Thất bại, mã lỗi=");
      Serial.print(client.state());
      Serial.println(" thử lại sau 5s...");
      delay(5000);
    }
  }
}

// Hàm đọc nhiệt độ theo ưu tiên MS5611, fallback DHT12
String readTemperature() {
  if (ms5611Available) {
    if (ms5611.read()) {   // đọc thành công
      float temp = ms5611.getTemperature();
      if (!isnan(temp)) {
        return String(temp, 2);
      }
    }
    // Nếu lỗi đọc MS5611, dùng DHT12 đọc tạm
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
  } 
}
  else {
    Serial.println("ms5611.read() failed.");
  }
}

// Đọc độ ẩm bằng DHT12
String readDHTHumidity() {
  float h = dht.readHumidity();
  return isnan(h) ? "--" : String(h, 2);
}

String readLux() {
  float lux = luxSensor.lightStrengthLux();
  //Serial.print(lux);
  if (lux < 0) return "--";
  // Chuyển thành int nếu muốn, hoặc làm tròn 2 số thập phân
  return String(int(lux)); // Không đơn vị
}

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

void setup() {
  Serial.begin(115200);
  delay(1000);


  pinMode(LEDPIN, OUTPUT);
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);

  Wire.begin(12,11);  //GY63
  Wire2.begin(18, 17);  // Light Sensor
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
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Cập nhật giá trị cảm biến
  updateAllSensors();

  // Chuẩn bị payload định dạng riêng từng cảm biến
  String payload_temp = String(cluster_code) + "_T_" + temperature;
  String payload_humid = String(cluster_code) + "_H_" + humidity;
  String payload_press = String(cluster_code) + "_P_" + ms5611Pressure;
  String payload_lux = String(cluster_code) + "_L_" + luxValue;
  
  // Gửi từng giá trị lên topic sensor/data
  client.publish(mqtt_topic, payload_temp.c_str());
  delay(100); // delay nhỏ để tránh gửi dồn
  client.publish(mqtt_topic, payload_humid.c_str());
  delay(100);
  client.publish(mqtt_topic, payload_press.c_str());
  delay(100);
  client.publish(mqtt_topic, payload_lux.c_str());

  Serial.print("Sent MQTT Temperature: ");
  Serial.println(payload_temp);
  Serial.print("Sent MQTT Humidity: ");
  Serial.println(payload_humid);
  Serial.print("Sent MQTT Pressure: ");
  Serial.println(payload_press);
  Serial.print("Sent MQTT Lux: ");
  Serial.println(payload_lux);
  digitalWrite(LEDPIN, HIGH);
  delay(2500);
  digitalWrite(LEDPIN, LOW);
}
