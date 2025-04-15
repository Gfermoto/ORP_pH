#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
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
const int ledPin = 2;    // GPIO2 для светодиода
const int bootButton = 0; // Кнопка BOOT

// Константы для расчетов
#define VCC 3.3          // Напряжение питания ESP32
#define OFFSET 256       // Смещение для ORP (калибруется)
#define PH_OFFSET 0      // Смещение для pH (калибруется)
#define PH_SCALE 1.0     // Множитель для pH (калибруется)
#define ORP_SCALE 1.0    // Множитель для ORP (калибруется)

// Настройки по умолчанию
#define DEFAULT_UPDATE_INTERVAL 5000  // Интервал обновления в мс
#define DEFAULT_DEVICE_NAME "WaterSensor"
#define BOOT_HOLD_TIME 3000 // Время удержания кнопки в мс

// Массивы для усреднения значений
#define ARRAY_LENGTH 40
int phArray[ARRAY_LENGTH];
int orpArray[ARRAY_LENGTH];
int phArrayIndex = 0;
int orpArrayIndex = 0;

// Константы для светодиода
#define LED_BLINK_INTERVAL 1000  // Интервал мигания в мс
unsigned long lastLedBlink = 0;
bool ledState = false;

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
  char deviceName[32];
  char wifiSSID[32];
  char wifiPassword[64];
  char mqttServer[64];
  int mqttPort;
  char mqttUser[32];
  char mqttPassword[64];
  int updateInterval;
  Calibration calibration;
};

Settings settings;
SensorValues sensorValues;

// Таймеры
unsigned long lastSensorRead = 0;
unsigned long bootButtonPressTime = 0;
bool bootButtonPressed = false;

// HTML страницы
const char* index_html = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>Water Quality Sensor</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
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

    .tabs {
      display: flex;
      margin-bottom: 20px;
      border-bottom: 1px solid #ddd;
    }

    .tab {
      padding: 10px 20px;
      cursor: pointer;
      border: none;
      background: none;
      color: #666;
    }

    .tab.active {
      color: var(--primary-color);
      border-bottom: 2px solid var(--primary-color);
    }

    .tab-content {
      display: none;
    }

    .tab-content.active {
      display: block;
    }

    .checkbox-container {
      margin: 10px 0;
      display: flex;
      align-items: center;
    }
    
    .checkbox-container input[type="checkbox"] {
      margin-right: 10px;
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

    <div class="tabs">
      <button class="tab active" onclick="showTab('readings')">Показания</button>
      <button class="tab" onclick="showTab('settings')">Настройки</button>
      <button class="tab" onclick="showTab('about')">О проекте</button>
    </div>
    
    <div id="readings" class="tab-content active">
      <div class="card">
        <div class="sensor">
          <h2>pH Sensor</h2>
          <div class="value" id="phValue">--</div>
          <div class="raw-value">Сырое значение: <span id="phRaw">--</span></div>
          <div class="calibration">
            <label>Калибровка pH</label>
            <input type="number" id="phCalibration" step="0.1" placeholder="Смещение">
            <input type="number" id="phScale" step="0.1" placeholder="Множитель">
            <div class="checkbox-container">
              <input type="checkbox" id="resetPhCalibration">
              <label for="resetPhCalibration">Сбросить калибровку pH</label>
            </div>
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
            <div class="checkbox-container">
              <input type="checkbox" id="resetOrpCalibration">
              <label for="resetOrpCalibration">Сбросить калибровку ORP</label>
            </div>
          </div>
        </div>
        
        <button class="button" onclick="saveCalibration()">Сохранить калибровку</button>
        <div id="status" class="status" style="display: none;"></div>
      </div>
    </div>

    <div id="settings" class="tab-content">
      <div class="card">
        <h2>Настройки устройства</h2>
        <div class="calibration">
          <label>Имя устройства</label>
          <input type="text" id="deviceName" placeholder="Имя устройства">
          
          <label>Интервал обновления (мс)</label>
          <input type="number" id="updateInterval" placeholder="Интервал обновления">
        </div>

        <h2>Настройки WiFi</h2>
        <div class="calibration">
          <label>SSID</label>
          <input type="text" id="wifiSSID" placeholder="SSID">
          
          <label>Пароль</label>
          <input type="password" id="wifiPassword" placeholder="Пароль">
        </div>

        <h2>Настройки MQTT</h2>
        <div class="calibration">
          <label>Сервер</label>
          <input type="text" id="mqttServer" placeholder="MQTT сервер">
          
          <label>Порт</label>
          <input type="number" id="mqttPort" placeholder="Порт">
          
          <label>Пользователь</label>
          <input type="text" id="mqttUser" placeholder="Пользователь">
          
          <label>Пароль</label>
          <input type="password" id="mqttPassword" placeholder="Пароль">
        </div>
        
        <button class="button" onclick="saveSettings()">Сохранить настройки</button>
        <div id="settingsStatus" class="status" style="display: none;"></div>
      </div>
    </div>

    <div id="about" class="tab-content">
      <div class="card">
        <h2>О проекте</h2>
        <p>Система мониторинга качества воды в бассейне с использованием ESP32 и датчиков pH/ORP.</p>
        
        <h3>Основные функции:</h3>
        <ul>
          <li>Измерение pH и ORP в реальном времени</li>
          <li>Калибровка датчиков через веб-интерфейс</li>
          <li>Интеграция с Home Assistant через MQTT</li>
          <li>OTA обновление прошивки</li>
          <li>Настраиваемый интервал обновления</li>
          <li>Управление светодиодом</li>
        </ul>

        <h3>Технические характеристики:</h3>
        <ul>
          <li>ESP32-WROOM-32</li>
          <li>pH датчик: 0-14 pH</li>
          <li>ORP датчик: -2000 до 2000 mV</li>
          <li>WiFi: 802.11 b/g/n</li>
          <li>Питание: 5V USB</li>
        </ul>

        <h3>OTA Обновление</h3>
        <p>Для обновления прошивки по воздуху:</p>
        <form method='POST' action='/update' enctype='multipart/form-data'>
          <input type='file' name='update'>
          <button class="button" type='submit'>Загрузить прошивку</button>
        </form>
        <p><strong>Важно:</strong></p>
        <ul>
          <li>Не отключайте питание во время обновления</li>
          <li>Дождитесь перезагрузки устройства</li>
        </ul>
      </div>
    </div>
  </div>
  
  <script>
    function showTab(tabName) {
      // Скрыть все вкладки
      document.querySelectorAll('.tab-content').forEach(tab => {
        tab.classList.remove('active');
      });
      document.querySelectorAll('.tab').forEach(tab => {
        tab.classList.remove('active');
      });
      
      // Показать выбранную вкладку
      document.getElementById(tabName).classList.add('active');
      document.querySelector(`.tab[onclick="showTab('${tabName}')"]`).classList.add('active');
    }

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
      const resetPh = document.getElementById('resetPhCalibration').checked;
      const resetOrp = document.getElementById('resetOrpCalibration').checked;
      
      fetch('/calibrate?phCalibration=' + phCalibration + 
            '&phScale=' + phScale + 
            '&orpCalibration=' + orpCalibration + 
            '&orpScale=' + orpScale +
            '&resetPh=' + resetPh +
            '&resetOrp=' + resetOrp)
        .then(response => response.text())
        .then(data => {
          showStatus(data);
          updateValues();
          // Сбрасываем чекбоксы после сохранения
          document.getElementById('resetPhCalibration').checked = false;
          document.getElementById('resetOrpCalibration').checked = false;
        })
        .catch(error => {
          showStatus('Ошибка при сохранении калибровки', true);
          console.error('Error:', error);
        });
    }

    function saveSettings() {
      const settings = {
        deviceName: document.getElementById('deviceName').value,
        updateInterval: document.getElementById('updateInterval').value,
        wifiSSID: document.getElementById('wifiSSID').value,
        wifiPassword: document.getElementById('wifiPassword').value,
        mqttServer: document.getElementById('mqttServer').value,
        mqttPort: document.getElementById('mqttPort').value,
        mqttUser: document.getElementById('mqttUser').value,
        mqttPassword: document.getElementById('mqttPassword').value
      };

      fetch('/settings', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(settings)
      })
      .then(response => response.text())
      .then(data => {
        const status = document.getElementById('settingsStatus');
        status.textContent = data;
        status.style.display = 'block';
        setTimeout(() => {
          status.style.display = 'none';
        }, 3000);
      })
      .catch(error => {
        console.error('Error:', error);
      });
    }

    // Загрузка текущих настроек при открытии страницы
    fetch('/settings')
      .then(response => response.json())
      .then(data => {
        document.getElementById('deviceName').value = data.deviceName;
        document.getElementById('updateInterval').value = data.updateInterval;
        document.getElementById('wifiSSID').value = data.wifiSSID;
        document.getElementById('wifiPassword').value = data.wifiPassword;
        document.getElementById('mqttServer').value = data.mqttServer;
        document.getElementById('mqttPort').value = data.mqttPort;
        document.getElementById('mqttUser').value = data.mqttUser;
        document.getElementById('mqttPassword').value = data.mqttPassword;
      });

    setInterval(updateValues, 5000);
    updateValues();
  </script>
</body>
</html>
)rawliteral";

// В HTML странице калибровки исправляем отображение
const char* calibration_html = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>Калибровка датчиков</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 20px;
      background-color: #f5f5f5;
    }
    .container {
      max-width: 600px;
      margin: 0 auto;
      background: white;
      padding: 20px;
      border-radius: 8px;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    .sensor {
      margin-bottom: 20px;
      padding: 15px;
      border-radius: 8px;
      background-color: #f8f9fa;
    }
    .sensor h2 {
      color: #2196F3;
      margin-bottom: 10px;
    }
    .current-values {
      background-color: #e8f5e9;
      padding: 10px;
      border-radius: 4px;
      margin-bottom: 15px;
    }
    .calibration {
      margin-top: 15px;
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
      background-color: #2196F3;
      color: white;
      border: none;
      padding: 12px 24px;
      border-radius: 4px;
      cursor: pointer;
      font-size: 16px;
      margin-top: 10px;
    }
    .button:hover {
      background-color: #1976D2;
    }
    .reset-section {
      margin-top: 30px;
      padding-top: 20px;
      border-top: 1px solid #ddd;
    }
    .reset-button {
      background-color: #f44336;
      color: white;
      border: none;
      padding: 12px 24px;
      border-radius: 4px;
      cursor: pointer;
      font-size: 16px;
    }
    .reset-button:hover {
      background-color: #d32f2f;
    }
    .status {
      margin-top: 15px;
      padding: 10px;
      border-radius: 4px;
      display: none;
    }
    .success {
      background-color: #e8f5e9;
      color: #2e7d32;
    }
    .error {
      background-color: #ffebee;
      color: #c62828;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Калибровка датчиков</h1>
    
    <div class="sensor">
      <h2>pH Датчик</h2>
      <div class="current-values">
        <p>Текущее значение: <span id="currentPh">--</span></p>
        <p>Текущая калибровка:</p>
        <p>Смещение: <span id="currentPhOffset">--</span></p>
        <p>Множитель: <span id="currentPhScale">--</span></p>
      </div>
      <div class="calibration">
        <label>Смещение pH:</label>
        <input type="number" id="phCalibration" step="0.1" placeholder="Смещение">
        <label>Множитель pH:</label>
        <input type="number" id="phScale" step="0.1" placeholder="Множитель">
      </div>
    </div>
    
    <div class="sensor">
      <h2>ORP Датчик</h2>
      <div class="current-values">
        <p>Текущее значение: <span id="currentOrp">--</span> mV</p>
        <p>Текущая калибровка:</p>
        <p>Смещение: <span id="currentOrpOffset">--</span></p>
        <p>Множитель: <span id="currentOrpScale">--</span></p>
      </div>
      <div class="calibration">
        <label>Смещение ORP:</label>
        <input type="number" id="orpCalibration" step="0.1" placeholder="Смещение">
        <label>Множитель ORP:</label>
        <input type="number" id="orpScale" step="0.1" placeholder="Множитель">
      </div>
    </div>
    
    <button class="button" onclick="saveCalibration()">Сохранить калибровку</button>
    <div id="status" class="status"></div>
    
    <div class="reset-section">
      <h3>Сброс калибровки</h3>
      <p>Эта операция сбросит все значения калибровки на значения по умолчанию:</p>
      <ul>
        <li>Смещение pH: 0.0</li>
        <li>Множитель pH: 1.0</li>
        <li>Смещение ORP: 256.0</li>
        <li>Множитель ORP: 1.0</li>
      </ul>
      <button class="reset-button" onclick="resetCalibration()">Сбросить калибровку</button>
    </div>
  </div>
  
  <script>
    function updateValues() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          document.getElementById('currentPh').innerHTML = data.ph.toFixed(2);
          document.getElementById('currentOrp').innerHTML = data.orp.toFixed(0);
        });
      
      fetch('/calibration')
        .then(response => response.json())
        .then(data => {
          document.getElementById('currentPhOffset').innerHTML = data.phOffset.toFixed(1);
          document.getElementById('currentPhScale').innerHTML = data.phScale.toFixed(1);
          document.getElementById('currentOrpOffset').innerHTML = data.orpOffset.toFixed(1);
          document.getElementById('currentOrpScale').innerHTML = data.orpScale.toFixed(1);
          
          document.getElementById('phCalibration').value = data.phOffset;
          document.getElementById('phScale').value = data.phScale;
          document.getElementById('orpCalibration').value = data.orpOffset;
          document.getElementById('orpScale').value = data.orpScale;
        });
    }
    
    function showStatus(message, isError = false) {
      const status = document.getElementById('status');
      status.textContent = message;
      status.style.display = 'block';
      status.className = 'status ' + (isError ? 'error' : 'success');
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
    
    function resetCalibration() {
      if (confirm('Вы уверены, что хотите сбросить все значения калибровки?')) {
        fetch('/calibrate?reset=true')
          .then(response => response.text())
          .then(data => {
            showStatus(data);
            updateValues();
          })
          .catch(error => {
            showStatus('Ошибка при сбросе калибровки', true);
            console.error('Error:', error);
          });
      }
    }
    
    setInterval(updateValues, 5000);
    updateValues();
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  pinMode(bootButton, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  
  // Проверяем состояние кнопки BOOT при старте
  if (digitalRead(bootButton) == LOW) {
    bootButtonPressTime = millis();
    bootButtonPressed = true;
    
    while(digitalRead(bootButton) == LOW && (millis() - bootButtonPressTime < BOOT_HOLD_TIME)) {
      delay(100);
      digitalWrite(ledPin, !digitalRead(ledPin));
    }
    
    if(digitalRead(bootButton) == LOW) {
      resetWiFi();
      return;
    }
  }
  
  loadSettings();
  setupWiFi();
  setupMQTT();
  
  // Настройка OTA
  ArduinoOTA.setHostname(settings.deviceName);
  ArduinoOTA.setPassword("12345678");
  
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
    for(int i = 0; i < 3; i++) {
      digitalWrite(ledPin, HIGH);
      delay(100);
      digitalWrite(ledPin, LOW);
      delay(100);
    }
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    digitalWrite(ledPin, !digitalRead(ledPin));
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    for(int i = 0; i < 3; i++) {
      digitalWrite(ledPin, HIGH);
      delay(100);
      digitalWrite(ledPin, LOW);
      delay(100);
    }
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    for(int i = 0; i < 3; i++) {
      digitalWrite(ledPin, HIGH);
      delay(500);
      digitalWrite(ledPin, LOW);
      delay(500);
    }
  });
  
  ArduinoOTA.begin();
  
  // Настройка веб-сервера
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", index_html);
  });
  
  server.on("/data", HTTP_GET, []() {
    StaticJsonDocument<200> doc;
    doc["ph"] = sensorValues.phValue;
    doc["orp"] = sensorValues.orpValue;
    doc["ph_raw"] = sensorValues.phRaw;
    doc["orp_raw"] = sensorValues.orpRaw;
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
  });
  
  server.on("/settings", HTTP_GET, []() {
    StaticJsonDocument<512> doc;
    doc["deviceName"] = settings.deviceName;
    doc["updateInterval"] = settings.updateInterval;
    doc["wifiSSID"] = settings.wifiSSID;
    doc["wifiPassword"] = settings.wifiPassword;
    doc["mqttServer"] = settings.mqttServer;
    doc["mqttPort"] = settings.mqttPort;
    doc["mqttUser"] = settings.mqttUser;
    doc["mqttPassword"] = settings.mqttPassword;
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
  });
  
  server.on("/settings", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, server.arg("plain"));
      
      if (!error) {
        // Сохраняем настройки устройства
        if (doc.containsKey("deviceName")) {
          strlcpy(settings.deviceName, doc["deviceName"], sizeof(settings.deviceName));
        }
        
        if (doc.containsKey("updateInterval")) {
          settings.updateInterval = doc["updateInterval"];
        }
        
        // Сохраняем настройки WiFi
        if (doc.containsKey("wifiSSID")) {
          strlcpy(settings.wifiSSID, doc["wifiSSID"], sizeof(settings.wifiSSID));
        }
        
        if (doc.containsKey("wifiPassword")) {
          strlcpy(settings.wifiPassword, doc["wifiPassword"], sizeof(settings.wifiPassword));
        }
        
        // Сохраняем настройки MQTT
        if (doc.containsKey("mqttServer")) {
          strlcpy(settings.mqttServer, doc["mqttServer"], sizeof(settings.mqttServer));
        }
        
        if (doc.containsKey("mqttPort")) {
          settings.mqttPort = doc["mqttPort"];
        }
        
        if (doc.containsKey("mqttUser")) {
          strlcpy(settings.mqttUser, doc["mqttUser"], sizeof(settings.mqttUser));
        }
        
        if (doc.containsKey("mqttPassword")) {
          strlcpy(settings.mqttPassword, doc["mqttPassword"], sizeof(settings.mqttPassword));
        }
        
        saveSettings();
        server.send(200, "text/plain", "Настройки сохранены");
        
        // Перезагружаем WiFi если изменились настройки сети
        if (doc.containsKey("wifiSSID") || doc.containsKey("wifiPassword")) {
          WiFi.disconnect();
          setupWiFi();
        }
        
        // Перезагружаем MQTT если изменились настройки
        if (doc.containsKey("mqttServer") || doc.containsKey("mqttPort") || 
            doc.containsKey("mqttUser") || doc.containsKey("mqttPassword")) {
          mqttClient.disconnect();
          setupMQTT();
        }
      } else {
        server.send(400, "text/plain", "Ошибка при разборе JSON");
      }
    } else {
      server.send(400, "text/plain", "Отсутствует тело запроса");
    }
  });
  
  server.on("/calibration", HTTP_GET, []() {
    server.send(200, "text/html", calibration_html);
  });

  server.on("/calibration", HTTP_GET, []() {
    StaticJsonDocument<200> doc;
    doc["phOffset"] = settings.calibration.phOffset;
    doc["phScale"] = settings.calibration.phScale;
    doc["orpOffset"] = settings.calibration.orpOffset;
    doc["orpScale"] = settings.calibration.orpScale;
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
  });

  server.on("/calibrate", HTTP_GET, []() {
    if (server.hasArg("reset") && server.arg("reset") == "true") {
      settings.calibration.phOffset = PH_OFFSET;
      settings.calibration.phScale = PH_SCALE;
      settings.calibration.orpOffset = OFFSET;
      settings.calibration.orpScale = ORP_SCALE;
      saveSettings();
      server.send(200, "text/plain", "Калибровка сброшена");
      return;
    }
    
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
  
  // Добавляем обработчик для загрузки прошивки
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  
  server.begin();
}

void loop() {
  // Проверяем состояние кнопки BOOT
  if (digitalRead(bootButton) == LOW) {
    if (!bootButtonPressed) {
      bootButtonPressTime = millis();
      bootButtonPressed = true;
      digitalWrite(ledPin, HIGH);
    } else if (millis() - bootButtonPressTime > BOOT_HOLD_TIME) {
      resetWiFi();
      return;
    }
  } else {
    if (bootButtonPressed) {
      bootButtonPressed = false;
      digitalWrite(ledPin, WiFi.getMode() == WIFI_AP);
    }
  }
  
  ArduinoOTA.handle();
  server.handleClient();
  
  // Чтение данных с датчиков
  if (millis() - lastSensorRead > settings.updateInterval) {
    lastSensorRead = millis();
    
    // Чтение pH
    phArray[phArrayIndex++] = analogRead(phPin);
    if (phArrayIndex == ARRAY_LENGTH) phArrayIndex = 0;
    float phAvg = averageArray(phArray, ARRAY_LENGTH);
    sensorValues.phRaw = phAvg;
    
    float phVoltage = phAvg * (VCC / 4095.0);
    sensorValues.phValue = 7.0 - ((phVoltage - 2.5) / 0.18);
    sensorValues.phValue = (sensorValues.phValue + settings.calibration.phOffset) * settings.calibration.phScale;
    
    // Чтение ORP
    orpArray[orpArrayIndex++] = analogRead(orpPin);
    if (orpArrayIndex == ARRAY_LENGTH) orpArrayIndex = 0;
    float orpAvg = averageArray(orpArray, ARRAY_LENGTH);
    sensorValues.orpRaw = orpAvg;
    
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
      setupMQTT();
    }
    
    // Индикация работы
    if (WiFi.getMode() == WIFI_AP) {
      digitalWrite(ledPin, LOW);
      delay(100);
      digitalWrite(ledPin, HIGH);
    } else {
      digitalWrite(ledPin, HIGH);
      delay(100);
      digitalWrite(ledPin, LOW);
    }
  }
}

void setupWiFi() {
  if (strlen(settings.wifiSSID) == 0) {
    WiFi.mode(WIFI_AP);
    String apName = settings.deviceName;
    WiFi.softAP(apName.c_str());
    digitalWrite(ledPin, HIGH);
    return;
  }
  
  String hostName = settings.deviceName;
  hostName.replace("_", "-");
  WiFi.setHostname(hostName.c_str());
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(settings.wifiSSID, settings.wifiPassword);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    digitalWrite(ledPin, !digitalRead(ledPin));
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(ledPin, LOW);
  } else {
    WiFi.mode(WIFI_AP);
    String apName = settings.deviceName;
    WiFi.softAP(apName.c_str());
    digitalWrite(ledPin, HIGH);
  }
}

void setupMQTT() {
  mqttClient.setServer(settings.mqttServer, settings.mqttPort);
  if (mqttClient.connect(settings.deviceName, settings.mqttUser, settings.mqttPassword)) {
    Serial.println("MQTT connected");
  }
}

void loadSettings() {
  EEPROM.begin(512);
  EEPROM.get(0, settings);
  EEPROM.end();
  
  // Проверяем валидность настроек и устанавливаем значения по умолчанию если нужно
  if (settings.updateInterval < 1000 || settings.updateInterval > 60000) {
    settings.updateInterval = DEFAULT_UPDATE_INTERVAL;
  }
  
  if (settings.mqttPort <= 0 || settings.mqttPort > 65535) {
    settings.mqttPort = MQTT_PORT; // Стандартный порт MQTT
  }
  
  if (strlen(settings.deviceName) == 0 || strlen(settings.deviceName) >= sizeof(settings.deviceName)) {
    String defaultName = getDeviceName();
    memset(settings.deviceName, 0, sizeof(settings.deviceName));
    strncpy(settings.deviceName, defaultName.c_str(), sizeof(settings.deviceName) - 1);
    settings.deviceName[sizeof(settings.deviceName) - 1] = '\0';
  }
  
  // Обеспечиваем нулевое завершение всех строк
  settings.wifiSSID[sizeof(settings.wifiSSID) - 1] = '\0';
  settings.wifiPassword[sizeof(settings.wifiPassword) - 1] = '\0';
  settings.mqttServer[sizeof(settings.mqttServer) - 1] = '\0';
  settings.mqttUser[sizeof(settings.mqttUser) - 1] = '\0';
  settings.mqttPassword[sizeof(settings.mqttPassword) - 1] = '\0';
}

void saveSettings() {
  EEPROM.begin(512);
  EEPROM.put(0, settings);
  EEPROM.commit();
  EEPROM.end();
}

String getDeviceName() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char deviceName[20];
  snprintf(deviceName, sizeof(deviceName), "Water_%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(deviceName);
}

void resetWiFi() {
  digitalWrite(ledPin, HIGH);
  
  server.stop();
  WiFi.disconnect(true);
  delay(1000);
  
  // Очищаем настройки WiFi
  strncpy(settings.wifiSSID, "", sizeof(settings.wifiSSID) - 1);
  strncpy(settings.wifiPassword, "", sizeof(settings.wifiPassword) - 1);
  
  saveSettings();
  
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  
  String apName = settings.deviceName;
  WiFi.softAP(apName.c_str());
  
  server.begin();
  
  for (int i = 0; i < 5; i++) {
    digitalWrite(ledPin, LOW);
    delay(100);
    digitalWrite(ledPin, HIGH);
    delay(100);
  }
  
  delay(3000);
  ESP.restart();
}

float averageArray(int* arr, int number) {
  if (number <= 0) return 0;
  float sum = 0;
  for (int i = 0; i < number; i++) {
    sum += arr[i];
  }
  return sum / number;
} 