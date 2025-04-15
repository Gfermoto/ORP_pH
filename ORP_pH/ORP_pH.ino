#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

// Конфигурация WiFi
const char* AP_SSID = "WaterSensor";
const char* AP_PASSWORD = "12345678";
const char* MQTT_SERVER = "mqtt.local";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASSWORD = "";

// Конфигурация OTA
const char* OTA_HOSTNAME = "pool-sensor";
const char* OTA_PASSWORD = "12345678";

// Конфигурация пинов
const int phPin = 34;    // GPIO34 для pH датчика
const int orpPin = 35;   // GPIO35 для ORP датчика

// Константы для расчетов
#define VCC 3.3          // Напряжение питания ESP32
#define OFFSET 256       // Смещение для ORP (калибруется)
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
Preferences preferences;

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
    :root {
      --primary-color: #2196F3;
      --secondary-color: #1976D2;
      --background-color: #f5f5f5;
      --card-color: #ffffff;
      --text-color: #333333;
      --border-radius: 8px;
      --box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }

    * {
      margin: 0;
      padding: 0;
      box-sizing: border-box;
    }

    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background-color: var(--background-color);
      color: var(--text-color);
      line-height: 1.6;
      padding: 20px;
    }

    .container {
      max-width: 800px;
      margin: 0 auto;
    }

    .header {
      text-align: center;
      margin-bottom: 30px;
    }

    .header h1 {
      color: var(--primary-color);
      margin-bottom: 10px;
    }

    .card {
      background-color: var(--card-color);
      border-radius: var(--border-radius);
      box-shadow: var(--box-shadow);
      padding: 20px;
      margin-bottom: 20px;
    }

    .sensor {
      margin-bottom: 20px;
      padding: 15px;
      border-radius: var(--border-radius);
      background-color: var(--background-color);
    }

    .sensor h2 {
      color: var(--primary-color);
      margin-bottom: 10px;
    }

    .value {
      font-size: 36px;
      font-weight: bold;
      color: var(--primary-color);
      margin: 10px 0;
    }

    .raw-value {
      font-size: 14px;
      color: #666;
      margin-bottom: 10px;
    }

    .calibration {
      margin-top: 15px;
      padding-top: 15px;
      border-top: 1px solid #eee;
    }

    .calibration label {
      display: block;
      margin-bottom: 5px;
      color: #666;
    }

    .calibration input {
      width: 100%;
      padding: 8px;
      margin-bottom: 10px;
      border: 1px solid #ddd;
      border-radius: 4px;
    }

    .button {
      background-color: var(--primary-color);
      color: white;
      border: none;
      padding: 12px 24px;
      border-radius: var(--border-radius);
      cursor: pointer;
      font-size: 16px;
      transition: background-color 0.3s;
      width: 100%;
    }

    .button:hover {
      background-color: var(--secondary-color);
    }

    .status {
      text-align: center;
      margin-top: 20px;
      padding: 10px;
      border-radius: var(--border-radius);
      background-color: #e8f5e9;
      color: #2e7d32;
    }

    @media (max-width: 600px) {
      .container {
        padding: 10px;
      }
      
      .value {
        font-size: 28px;
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>Water Quality Sensor</h1>
      <p>Мониторинг качества воды в реальном времени</p>
    </div>
    
    <div class="card">
      <div class="sensor">
        <h2>pH Sensor</h2>
        <div class="value" id="phValue">--</div>
        <div class="raw-value">Сырое значение: <span id="phRaw">--</span></div>
        <div class="calibration">
          <label>Калибровка pH</label>
          <input type="number" id="phCalibration" step="0.1" placeholder="Смещение">
          <input type="number" id="phScale" step="0.1" placeholder="Множитель">
        </div>
      </div>
      
      <div class="sensor">
        <h2>ORP Sensor</h2>
        <div class="value" id="orpValue">--</div>
        <div class="raw-value">Сырое значение: <span id="orpRaw">--</span></div>
        <div class="calibration">
          <label>Калибровка ORP</label>
          <input type="number" id="orpCalibration" step="0.1" placeholder="Смещение">
          <input type="number" id="orpScale" step="0.1" placeholder="Множитель">
        </div>
      </div>
      
      <button class="button" onclick="saveCalibration()">Сохранить калибровку</button>
      <div id="status" class="status" style="display: none;"></div>
    </div>
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
        })
        .catch(error => {
          console.error('Error:', error);
        });
    }

    function showStatus(message, isError = false) {
      const status = document.getElementById('status');
      status.textContent = message;
      status.style.display = 'block';
      status.style.backgroundColor = isError ? '#ffebee' : '#e8f5e9';
      status.style.color = isError ? '#c62828' : '#2e7d32';
      setTimeout(() => {
        status.style.display = 'none';
      }, 3000);
    }

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
        .then(data => {
          showStatus(data);
          updateValues();
        })
        .catch(error => {
          showStatus('Ошибка при сохранении калибровки', true);
          console.error('Error:', error);
        });
    }

    setInterval(updateValues, 5000);
    updateValues();
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
  preferences.begin("water_sensor", false);
  loadSettings();
  
  // Настройка WiFi
  setupWiFi();
  
  // Настройка OTA
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  
  ArduinoOTA.begin();
  
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
      settings.calibration.phOffset = server.arg("phCalibration").toFloat();
      settings.calibration.phScale = server.arg("phScale").toFloat();
      settings.calibration.orpOffset = server.arg("orpCalibration").toFloat();
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
  ArduinoOTA.handle();
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
  preferences.begin("water_sensor", false);
  
  // Загрузка калибровочных значений
  settings.calibration.phOffset = preferences.getFloat("ph_offset", PH_OFFSET);
  settings.calibration.phScale = preferences.getFloat("ph_scale", PH_SCALE);
  settings.calibration.orpOffset = preferences.getFloat("orp_offset", OFFSET);
  settings.calibration.orpScale = preferences.getFloat("orp_scale", ORP_SCALE);
  
  // Загрузка сетевых настроек
  preferences.getString("wifi_ssid", settings.wifiSSID, 32);
  preferences.getString("wifi_pass", settings.wifiPassword, 64);
  preferences.getString("mqtt_server", settings.mqttServer, 64);
  settings.mqttPort = preferences.getInt("mqtt_port", MQTT_PORT);
  preferences.getString("mqtt_user", settings.mqttUser, 32);
  preferences.getString("mqtt_pass", settings.mqttPassword, 64);
  
  preferences.end();
}

void saveSettings() {
  preferences.begin("water_sensor", false);
  
  // Сохранение калибровочных значений
  preferences.putFloat("ph_offset", settings.calibration.phOffset);
  preferences.putFloat("ph_scale", settings.calibration.phScale);
  preferences.putFloat("orp_offset", settings.calibration.orpOffset);
  preferences.putFloat("orp_scale", settings.calibration.orpScale);
  
  // Сохранение сетевых настроек
  preferences.putString("wifi_ssid", settings.wifiSSID);
  preferences.putString("wifi_pass", settings.wifiPassword);
  preferences.putString("mqtt_server", settings.mqttServer);
  preferences.putInt("mqtt_port", settings.mqttPort);
  preferences.putString("mqtt_user", settings.mqttUser);
  preferences.putString("mqtt_pass", settings.mqttPassword);
  
  preferences.end();
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