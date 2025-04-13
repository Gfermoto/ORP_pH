#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

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

// Калибровочные значения
struct Calibration {
  float phCalibration = 0.0;
  float phScale = 1.0;
  float orpCalibration = 0.0;
  float orpScale = 1.0;
};

// Значения с датчиков
struct SensorValues {
  float phValue = 0.0;
  float phVoltage = 0.0;
  float orpValue = 0.0;
  float orpVoltage = 0.0;
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
    .button { background-color: #4CAF50; border: none; color: white; padding: 15px 32px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; border-radius: 5px; }
    .sensor { margin-bottom: 20px; }
  </style>
</head>
<body>
  <h1>Water Quality Sensor</h1>
  
  <div class="card">
    <div class="sensor">
      <h2>pH Sensor</h2>
      <div class="value" id="phValue">--</div>
      <div>Калибровка:</div>
      <input type="number" id="phCalibration" step="0.1" placeholder="Смещение">
      <input type="number" id="phScale" step="0.1" placeholder="Множитель">
    </div>
    
    <div class="sensor">
      <h2>ORP Sensor</h2>
      <div class="value" id="orpValue">--</div>
      <div>Калибровка:</div>
      <input type="number" id="orpCalibration" step="0.1" placeholder="Смещение">
      <input type="number" id="orpScale" step="0.1" placeholder="Множитель">
    </div>
    
    <button class="button" onclick="saveCalibration()">Сохранить калибровку</button>
  </div>
  
  <script>
    function updateValues() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          document.getElementById('phValue').innerHTML = data.ph.toFixed(2);
          document.getElementById('orpValue').innerHTML = data.orp.toFixed(0) + ' mV';
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
  
  // Чтение и обработка данных с датчиков
  int phSensorValue = analogRead(phPin);
  int orpSensorValue = analogRead(orpPin);
  
  // Преобразование в напряжение
  sensorValues.phVoltage = phSensorValue * (3.3 / 4095.0);
  sensorValues.orpVoltage = orpSensorValue * (3.3 / 4095.0);
  
  // Расчет значений с учетом калибровки
  sensorValues.phValue = (7.0 + ((2.5 - sensorValues.phVoltage) / 0.18) + 
                         settings.calibration.phCalibration) * settings.calibration.phScale;
  
  // Расчет ORP (примерная формула, может потребовать корректировки)
  sensorValues.orpValue = (sensorValues.orpVoltage * 1000) + settings.calibration.orpCalibration;
  sensorValues.orpValue *= settings.calibration.orpScale;
  
  // Отправка данных в MQTT
  if (mqttClient.connected()) {
    StaticJsonDocument<200> doc;
    doc["ph"] = sensorValues.phValue;
    doc["orp"] = sensorValues.orpValue;
    String output;
    serializeJson(doc, output);
    mqttClient.publish("homeassistant/sensor/water_sensor/state", output.c_str());
  } else {
    mqttConnect();
  }
  
  delay(5000);  // Обновление каждые 5 секунд
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