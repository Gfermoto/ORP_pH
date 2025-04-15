#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <SEN0161.h>

// Конфигурация WiFi
const char* AP_SSID = "WaterSensor";
const char* AP_PASSWORD = "12345678";
const char* MQTT_SERVER = "mqtt.local";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASSWORD = "";

// Конфигурация пинов
const int phPin = 34;    // GPIO34 для pH датчика
const int orpPin = 35;   // GPIO35 для ORP датчика

// Создание объекта для pH датчика
SEN0161 phSensor(phPin);

// Константы для расчетов
#define VCC 3.3          // Напряжение питания ESP32
#define OFFSET 0         // Смещение для ORP (калибруется)
#define PH_OFFSET 0      // Смещение для pH (калибруется)
#define PH_SCALE 1.0     // Множитель для pH (калибруется)
#define ORP_SCALE 1.0    // Множитель для ORP (калибруется)

// Массивы для усреднения значений
#define ARRAY_LENGTH 40
int phArray[ARRAY_LENGTH];
int orpArray[ARRAY_LENGTH];
int phArrayIndex = 0;
int orpArrayIndex = 0;

// Калибровочные значения
struct Calibration {
  float phOffset = PH_OFFSET;
  float phScale = PH_SCALE;
  float orpOffset = OFFSET;
  float orpScale = ORP_SCALE;
};

// Значения с датчиков
struct SensorValues {
  float phValue = 0.0;
  float orpValue = 0.0;
  float phRaw = 0.0;
  float orpRaw = 0.0;
};

// Объекты для работы с сетью
WebServer server(80);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Структура для хранения настроек
struct Settings {
  char wifiSSID[32];
  char wifiPassword[64];
  char mqttServer[64];
  int mqttPort;
  char mqttUser[32];
  char mqttPassword[64];
  Calibration calibration;
};

Settings settings;
SensorValues sensorValues;

// HTML страницы
const char* index_html = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>Water Quality Sensor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin: 0px auto; padding: 20px; }
    .card { background-color: #f1f1f1; padding: 20px; margin: 10px; border-radius: 10px; }
    .value { font-size: 48px; font-weight: bold; }
    .raw-value { font-size: 24px; color: #666; }
    .button { background-color: #4CAF50; border: none; color: white; padding: 15px 32px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; border-radius: 5px; }
    .sensor { margin-bottom: 20px; }
    .calibration { margin-top: 10px; }
  </style>
</head>
<body>
  <h1>Water Quality Sensor</h1>
  
  <div class="card">
    <div class="sensor">
      <h2>pH Sensor</h2>
      <div class="value" id="phValue">--</div>
      <div class="raw-value">Сырое значение: <span id="phRaw">--</span></div>
      <div class="calibration">
        <div>Калибровка:</div>
        <input type="number" id="phCalibration" step="0.1" placeholder="Смещение">
        <input type="number" id="phScale" step="0.1" placeholder="Множитель">
      </div>
    </div>
    
    <div class="sensor">
      <h2>ORP Sensor</h2>
      <div class="value" id="orpValue">--</div>
      <div class="raw-value">Сырое значение: <span id="orpRaw">--</span></div>
      <div class="calibration">
        <div>Калибровка:</div>
        <input type="number" id="orpCalibration" step="0.1" placeholder="Смещение">
        <input type="number" id="orpScale" step="0.1" placeholder="Множитель">
      </div>
    </div>
    
    <button class="button" onclick="saveCalibration()">Сохранить калибровку</button>
  </div>
  
  <script>
    function updateValues() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          document.getElementById('phValue').innerHTML = data.ph.toFixed(2);
          document.getElementById('phRaw').innerHTML = data.ph_raw;
          document.getElementById('orpValue').innerHTML = data.orp.toFixed(0) + ' mV';
          document.getElementById('orpRaw').innerHTML = data.orp_raw;
        });
    }
    setInterval(updateValues, 5000);
    updateValues();

    function saveCalibration() {
      const phCalibration = document.getElementById('phCalibration').value;
      const phScale = document.getElementById('phScale').value;
      const orpCalibration = document.getElementById('orpCalibration').value;
      const orpScale = document.getElementById('orpScale').value;
      
      fetch('/calibrate?phCalibration=' + phCalibration + 
            '&phScale=' + phScale + 
            '&orpCalibration=' + orpCalibration + 
            '&orpScale=' + orpScale)
        .then(response => response.text())
        .then(data => alert(data));
    }
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  
  // Настройка аналоговых входов
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  
  // Инициализация EEPROM
  EEPROM.begin(512);
  loadSettings();
  
  // Настройка WiFi
  setupWiFi();
  
  // Настройка MQTT
  mqttClient.setServer(settings.mqttServer, settings.mqttPort);
  
  // Настройка веб-сервера
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", index_html);
  });
  
  server.on("/data", HTTP_GET, []() {
    StaticJsonDocument<200> doc;
    doc["ph"] = sensorValues.phValue;
    doc["orp"] = sensorValues.orpValue;
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
  });
  
  server.on("/calibrate", HTTP_GET, []() {
    if (server.hasArg("phCalibration") && server.hasArg("phScale") &&
        server.hasArg("orpCalibration") && server.hasArg("orpScale")) {
      settings.calibration.phCalibration = server.arg("phCalibration").toFloat();
      settings.calibration.phScale = server.arg("phScale").toFloat();
      settings.calibration.orpCalibration = server.arg("orpCalibration").toFloat();
      settings.calibration.orpScale = server.arg("orpScale").toFloat();
      saveSettings();
      server.send(200, "text/plain", "Калибровка сохранена");
    } else {
      server.send(400, "text/plain", "Ошибка: не указаны параметры калибровки");
    }
  });
  
  server.begin();
}

void loop() {
  server.handleClient();
  
  // Чтение pH
  phArray[phArrayIndex++] = analogRead(phPin);
  if (phArrayIndex == ARRAY_LENGTH) {
    phArrayIndex = 0;
  }
  float phAvg = averageArray(phArray, ARRAY_LENGTH);
  sensorValues.phRaw = phAvg;
  
  // Расчет pH (формула из документации Seeed Studio)
  // E = 59.16 (mV/pH)
  float phVoltage = phAvg * (VCC / 4095.0);  // Преобразование в напряжение
  sensorValues.phValue = 7.0 - ((phVoltage - 2.5) / 0.18);  // Базовая формула
  sensorValues.phValue = (sensorValues.phValue + settings.calibration.phOffset) * settings.calibration.phScale;
  
  // Чтение ORP
  orpArray[orpArrayIndex++] = analogRead(orpPin);
  if (orpArrayIndex == ARRAY_LENGTH) {
    orpArrayIndex = 0;
  }
  float orpAvg = averageArray(orpArray, ARRAY_LENGTH);
  sensorValues.orpRaw = orpAvg;
  
  // Расчет ORP (формула из документации Seeed Studio)
  // ORP = ((30 * Vcc * 1000) - (75 * avg * Vcc * 1000/1024))/75 - OFFSET
  sensorValues.orpValue = ((30 * VCC * 1000) - (75 * orpAvg * VCC * 1000/4095))/75 - settings.calibration.orpOffset;
  sensorValues.orpValue *= settings.calibration.orpScale;
  
  // Отправка данных в MQTT
  if (mqttClient.connected()) {
    StaticJsonDocument<200> doc;
    doc["ph"] = sensorValues.phValue;
    doc["orp"] = sensorValues.orpValue;
    doc["ph_raw"] = sensorValues.phRaw;
    doc["orp_raw"] = sensorValues.orpRaw;
    String output;
    serializeJson(doc, output);
    mqttClient.publish("homeassistant/sensor/water_sensor/state", output.c_str());
  } else {
    mqttConnect();
  }
  
  delay(20);  // Интервал между измерениями 20мс
}

void setupWiFi() {
  // Попытка подключения к сохраненной сети
  WiFi.begin(settings.wifiSSID, settings.wifiPassword);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  
  // Если не удалось подключиться, создаем точку доступа
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.softAP(AP_SSID, AP_PASSWORD);
  }
}

void mqttConnect() {
  if (!mqttClient.connected()) {
    if (mqttClient.connect("WaterSensor", settings.mqttUser, settings.mqttPassword)) {
      // Публикация конфигурации для Home Assistant
      StaticJsonDocument<512> config;
      
      // Конфигурация pH сенсора
      config["name"] = "pH Sensor";
      config["device_class"] = "ph";
      config["state_topic"] = "homeassistant/sensor/water_sensor/state";
      config["unit_of_measurement"] = "pH";
      config["value_template"] = "{{ value_json.ph }}";
      String output;
      serializeJson(config, output);
      mqttClient.publish("homeassistant/sensor/ph_sensor/config", output.c_str());
      
      // Конфигурация ORP сенсора
      config["name"] = "ORP Sensor";
      config["device_class"] = "voltage";
      config["state_topic"] = "homeassistant/sensor/water_sensor/state";
      config["unit_of_measurement"] = "mV";
      config["value_template"] = "{{ value_json.orp }}";
      serializeJson(config, output);
      mqttClient.publish("homeassistant/sensor/orp_sensor/config", output.c_str());
    }
  }
}

void loadSettings() {
  EEPROM.get(0, settings);
  if (isnan(settings.calibration.phCalibration)) {
    settings.calibration.phCalibration = 0.0;
    settings.calibration.phScale = 1.0;
    settings.calibration.orpCalibration = 0.0;
    settings.calibration.orpScale = 1.0;
    strcpy(settings.wifiSSID, "");
    strcpy(settings.wifiPassword, "");
    strcpy(settings.mqttServer, MQTT_SERVER);
    settings.mqttPort = MQTT_PORT;
    strcpy(settings.mqttUser, MQTT_USER);
    strcpy(settings.mqttPassword, MQTT_PASSWORD);
  }
}

void saveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
}

// Функция для усреднения массива значений
float averageArray(int* arr, int number) {
  if (number <= 0) {
    return 0;
  }
  
  float sum = 0;
  for (int i = 0; i < number; i++) {
    sum += arr[i];
  }
  return sum / number;
} 