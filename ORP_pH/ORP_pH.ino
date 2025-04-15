#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include "esp_mac.h"
#include "version.h"

// Константы для настроек по умолчанию
const char* DEFAULT_SSID = "ORP_pH_AP";
const char* DEFAULT_PASSWORD = "12345678";
const char* DEFAULT_MQTT_SERVER = "192.168.1.100";
const int DEFAULT_MQTT_PORT = 1883;
const char* DEFAULT_MQTT_USER = "";
const char* DEFAULT_MQTT_PASSWORD = "";

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

// Настройки устройства по умолчанию
#define DEFAULT_UPDATE_INTERVAL 5000  // Интервал обновления датчиков в мс (5 секунд)
#define DEBOUNCE_DELAY 50
#define BOOT_HOLD_TIME_WIFI_RESET 3000
#define BOOT_HOLD_TIME_FACTORY_RESET 10000
#define WIFI_CONNECT_TIMEOUT 5000 // Уменьшаем таймаут до 5 секунд
#define WIFI_RECONNECT_ATTEMPTS 3 // Количество попыток подключения

// Массивы для усреднения значений
#define ARRAY_LENGTH 40
int phArray[ARRAY_LENGTH];
int orpArray[ARRAY_LENGTH];
int phArrayIndex = 0;
int orpArrayIndex = 0;

// Константы для расчетов и калибровки по умолчанию
#define VCC 3.3             // Напряжение питания ESP32 (важно для АЦП)
#define DEFAULT_ORP_OFFSET 0.0 // Смещение для ORP (калибруется) - уточнено
#define DEFAULT_PH_OFFSET 0.0  // Смещение для pH (калибруется)
#define DEFAULT_PH_SCALE 1.0   // Множитель для pH (калибруется)
#define DEFAULT_ORP_SCALE 1.0  // Множитель для ORP (калибруется)

// Калибровочные значения
struct Calibration {
  float phOffset = DEFAULT_PH_OFFSET;
  float phScale = DEFAULT_PH_SCALE;
  float orpOffset = DEFAULT_ORP_OFFSET;
  float orpScale = DEFAULT_ORP_SCALE;
};

// Значения с датчиков
struct SensorValues {
  float phValue = 7.0; // Начальные значения
  float orpValue = 0.0;
  float phRaw = 0.0;
  float orpRaw = 0.0;
};

// Объекты для работы с сетью
WebServer server(80);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Структура для хранения настроек в EEPROM
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
  uint16_t settingsVersion; // Добавим версию для проверки
};
Settings settings;
const uint16_t CURRENT_SETTINGS_VERSION = 0x01; // Версия структуры настроек

SensorValues sensorValues;

// Таймеры и флаги
unsigned long lastSensorRead = 0;
unsigned long lastMQTTReconnectAttempt = 0;
const long MQTT_RECONNECT_INTERVAL = 5000; // Интервал попыток переподключения MQTT

// --- Прототипы функций ---
void loadSettings();
void saveSettings();
void setupWiFi();
bool setupMQTT();
void resetWiFi();
void factoryReset();
String getDeviceName();
void handleRoot();
void handleSettings();
void handleCalibration();
void handleAbout();
void handleUpdateFirmware();
void handleDoUpdate();
void handleNotFound();
String getNavigationMenu(String activePage);
String getCSS();
void readSensors();
void publishSensorData();
// --------------------------

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n\nBooting ORP/pH Sensor..."));

  pinMode(bootButton, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  // Проверка кнопки BOOT с антидребезгом
  unsigned long pressStartTime = 0;
  bool resetTriggered = false;
  
  if (digitalRead(bootButton) == LOW) {
    delay(DEBOUNCE_DELAY); // Антидребезг
    if (digitalRead(bootButton) == LOW) { // Проверяем, что кнопка все еще нажата
      pressStartTime = millis();
      Serial.println(F("BOOT button held... Detecting hold duration."));
      
      // Индикация нажатия кнопки
      for(int i = 0; i < 3; i++) {
        digitalWrite(ledPin, HIGH);
        delay(100);
        digitalWrite(ledPin, LOW);
        delay(100);
      }
      
      while (digitalRead(bootButton) == LOW) {
        unsigned long holdTime = millis() - pressStartTime;
        
        // Индикация длительности нажатия
        if (holdTime % 1000 < 100) {
          digitalWrite(ledPin, HIGH);
        } else {
          digitalWrite(ledPin, LOW);
        }
        
        if (holdTime > BOOT_HOLD_TIME_FACTORY_RESET) {
          Serial.println(F("Factory reset triggered"));
          resetTriggered = true;
          factoryReset();
          break;
        } else if (holdTime > BOOT_HOLD_TIME_WIFI_RESET) {
          Serial.println(F("WiFi reset triggered"));
          resetTriggered = true;
          resetWiFi();
          break;
        }
        delay(50);
      }
    }
  }

  // Инициализация и основная логика setup только если не было сброса
  if (!resetTriggered) {
    Serial.println(F("Proceeding with normal boot sequence."));
    EEPROM.begin(sizeof(Settings)); // Размер структуры Settings
    loadSettings(); // Загружаем настройки (могут быть дефолтные)
    // EEPROM.end(); // Закроем после всех операций в setup, если нужно

    setupWiFi(); // Настраиваем WiFi
    setupMQTT(); // Настраиваем MQTT

    // --- Настройка OTA ---
    ArduinoOTA.setHostname(settings.deviceName);
    // ArduinoOTA.setPassword("ваш_пароль_ота"); // Раскомментируйте, если нужен пароль
    ArduinoOTA.onStart([]() {
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      Serial.println("Start updating " + type);
      mqttClient.disconnect();
      server.stop(); // Останавливаем веб-сервер перед OTA
      for(int i = 0; i < 3; i++) { digitalWrite(ledPin, HIGH); delay(50); digitalWrite(ledPin, LOW); delay(50); }
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      digitalWrite(ledPin, !digitalRead(ledPin));
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
      for(int i = 0; i < 3; i++) { digitalWrite(ledPin, HIGH); delay(100); digitalWrite(ledPin, LOW); delay(100); }
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
      else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
      else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
      else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
      else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
      for(int i = 0; i < 5; i++) { digitalWrite(ledPin, HIGH); delay(200); digitalWrite(ledPin, LOW); delay(200); }
      ESP.restart();
    });
    ArduinoOTA.begin();
    Serial.println(F("OTA Ready"));
    Serial.printf("OTA Hostname: %s\n", settings.deviceName);

    // --- Настройка WebServer ---
    server.on("/", HTTP_GET, handleRoot);
    server.on("/settings", HTTP_GET, handleSettings);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/calibration", HTTP_GET, handleCalibration);
    server.on("/calibration", HTTP_POST, handleCalibration);
    server.on("/about", HTTP_GET, handleAbout);
    // Обработчики для OTA через веб-интерфейс
    server.on("/update", HTTP_GET, handleUpdateFirmware); // Страница для загрузки
    server.on("/doUpdate", HTTP_POST, handleDoUpdate, []() { // Обработка загрузки файла
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Автоопределение размера
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) { // true to set the size to the current progress
                Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        } else {
             Serial.printf("Update Failed Unexpectedly (likely broken connection): Status = %d\n", upload.status);
              Update.abort(); // Прерываем обновление если что-то пошло не так
        }
    });
    server.onNotFound(handleNotFound); // Обработчик 404
    server.begin();
    Serial.println(F("HTTP server started"));
    if (WiFi.getMode() == WIFI_AP) {
        Serial.printf("Access AP at SSID: %s\n", settings.deviceName);
        Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    } else {
         // Создаем hostname для mDNS
         String hostname = String(settings.deviceName);
         hostname.toLowerCase(); // mDNS лучше с lowercase
         hostname.replace("_", "-");
         Serial.printf("Access Web UI via http://%s.local or http://%s\n", hostname.c_str(), WiFi.localIP().toString().c_str());
    }
  } else {
    Serial.println(F("Reset was triggered, halting normal boot. Should have restarted already."));
    while(true) { delay(1000); } // Остановка
  }
}

void loop() {
  ArduinoOTA.handle(); // Обработка OTA запросов
  server.handleClient(); // Обработка HTTP запросов

  // Переподключение MQTT при разрыве
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMQTTReconnectAttempt > MQTT_RECONNECT_INTERVAL) {
      lastMQTTReconnectAttempt = now;
      if (setupMQTT()) { // setupMQTT возвращает true при успехе
        lastMQTTReconnectAttempt = 0; // Сбрасываем таймер при успехе
      }
    }
  }
    mqttClient.loop(); // Поддержание MQTT соединения и обработка входящих сообщений (если есть подписки)

  // Чтение данных с датчиков и отправка MQTT
  if (millis() - lastSensorRead >= settings.updateInterval) {
    lastSensorRead = millis();
    readSensors();
    publishSensorData();

    // Индикация состояния светодиодом
    if (WiFi.getMode() == WIFI_AP) {
      // Режим точки доступа - медленное мигание
      static unsigned long lastBlink = 0;
      if (millis() - lastBlink > 1000) {
        digitalWrite(ledPin, !digitalRead(ledPin));
        lastBlink = millis();
      }
    } else if (WiFi.status() == WL_CONNECTED) {
      // Подключено к WiFi - короткая вспышка при отправке данных
      digitalWrite(ledPin, HIGH);
      delay(50);
      digitalWrite(ledPin, LOW);
    } else {
      // Попытка подключения - быстрое мигание
      static unsigned long lastBlink = 0;
      if (millis() - lastBlink > 200) {
        digitalWrite(ledPin, !digitalRead(ledPin));
        lastBlink = millis();
      }
    }
  }
}

// --- Реализация функций ---

// Загрузка настроек из EEPROM
void loadSettings() {
  Serial.println(F("Loading settings from EEPROM..."));
  Settings loadedSettings; // Временная структура для чтения
  EEPROM.get(0, loadedSettings);

  // Проверяем версию настроек (или "магическое число")
  if (loadedSettings.settingsVersion != CURRENT_SETTINGS_VERSION) {
    Serial.println(F("Settings version mismatch or EEPROM empty/corrupted. Loading default settings."));
    // Устанавливаем значения по умолчанию
    settings.settingsVersion = CURRENT_SETTINGS_VERSION;
    String defaultName = getDeviceName();
    strlcpy(settings.deviceName, defaultName.c_str(), sizeof(settings.deviceName));
    strlcpy(settings.wifiSSID, "", sizeof(settings.wifiSSID));
    strlcpy(settings.wifiPassword, "", sizeof(settings.wifiPassword));
    strlcpy(settings.mqttServer, "", sizeof(settings.mqttServer)); // Пустой сервер по умолчанию
    settings.mqttPort = 1883;
    strlcpy(settings.mqttUser, "", sizeof(settings.mqttUser));
    strlcpy(settings.mqttPassword, "", sizeof(settings.mqttPassword));
    settings.updateInterval = DEFAULT_UPDATE_INTERVAL;
    settings.calibration.phOffset = DEFAULT_PH_OFFSET;
    settings.calibration.phScale = DEFAULT_PH_SCALE;
    settings.calibration.orpOffset = DEFAULT_ORP_OFFSET;
    settings.calibration.orpScale = DEFAULT_ORP_SCALE;
    // Сохраняем дефолтные настройки обратно в EEPROM
    EEPROM.put(0, settings);
    if (!EEPROM.commit()) {
      Serial.println(F("ERROR: EEPROM commit failed while saving default settings!"));
    } else {
        Serial.println(F("Default settings saved to EEPROM."));
    }
  } else {
    // Версия совпадает, копируем загруженные настройки
    Serial.println(F("Valid settings found in EEPROM."));
    memcpy(&settings, &loadedSettings, sizeof(Settings));
  }

  // Дополнительная проверка и null-терминация (на всякий случай)
  settings.deviceName[sizeof(settings.deviceName) - 1] = '\0';
  settings.wifiSSID[sizeof(settings.wifiSSID) - 1] = '\0';
  settings.wifiPassword[sizeof(settings.wifiPassword) - 1] = '\0';
  settings.mqttServer[sizeof(settings.mqttServer) - 1] = '\0';
  settings.mqttUser[sizeof(settings.mqttUser) - 1] = '\0';
  settings.mqttPassword[sizeof(settings.mqttPassword) - 1] = '\0';

  // Валидация загруженных значений (даже если версия совпала)
   if (settings.updateInterval < 1000 || settings.updateInterval > 3600000) {
       Serial.println(F("Warning: Invalid update interval found, setting default."));
       settings.updateInterval = DEFAULT_UPDATE_INTERVAL;
       saveSettings(); // Сохраняем исправление
   }
    if (settings.mqttPort <= 0 || settings.mqttPort > 65535) {
        Serial.println(F("Warning: Invalid MQTT port found, setting default."));
        settings.mqttPort = DEFAULT_MQTT_PORT;
        saveSettings(); // Сохраняем исправление
    }
     if (strlen(settings.deviceName) == 0) {
        Serial.println(F("Warning: Device name is empty, generating default."));
        String defaultName = getDeviceName();
        strlcpy(settings.deviceName, defaultName.c_str(), sizeof(settings.deviceName));
        saveSettings(); // Сохраняем исправление
     }
     // Проверка калибровочных значений на NaN/Inf
     if (isnan(settings.calibration.phOffset) || isinf(settings.calibration.phOffset)) { settings.calibration.phOffset = DEFAULT_PH_OFFSET; saveSettings(); }
     if (isnan(settings.calibration.phScale) || isinf(settings.calibration.phScale) || settings.calibration.phScale == 0) { settings.calibration.phScale = DEFAULT_PH_SCALE; saveSettings();}
     if (isnan(settings.calibration.orpOffset) || isinf(settings.calibration.orpOffset)) { settings.calibration.orpOffset = DEFAULT_ORP_OFFSET; saveSettings();}
     if (isnan(settings.calibration.orpScale) || isinf(settings.calibration.orpScale) || settings.calibration.orpScale == 0) { settings.calibration.orpScale = DEFAULT_ORP_SCALE; saveSettings();}


  Serial.println(F("Settings loaded successfully."));
  Serial.printf("Device Name: %s\n", settings.deviceName);
  Serial.printf("WiFi SSID: %s\n", settings.wifiSSID);
  Serial.printf("MQTT Server: %s:%d\n", settings.mqttServer, settings.mqttPort);
  Serial.printf("Update Interval: %d ms\n", settings.updateInterval);
}

// Сохранение настроек в EEPROM
void saveSettings() {
  Serial.println(F("Saving settings to EEPROM..."));
  // Убедимся, что версия актуальна перед сохранением
  settings.settingsVersion = CURRENT_SETTINGS_VERSION;
  EEPROM.put(0, settings);
  if (EEPROM.commit()) {
    Serial.println(F("EEPROM commit successful."));
  } else {
    Serial.println(F("ERROR: EEPROM commit failed!"));
  }
}

// Настройка WiFi
void setupWiFi() {
  Serial.println(F("Setting up WiFi..."));
  if (strlen(settings.wifiSSID) == 0) {
    // Запуск в режиме точки доступа
    Serial.println(F("No SSID configured. Starting Access Point..."));
    WiFi.mode(WIFI_AP);
    String apName = String(settings.deviceName);
    WiFi.softAP(apName.c_str());
    Serial.printf("AP SSID: %s\n", apName.c_str());
    Serial.printf("AP IP address: %s\n", WiFi.softAPIP().toString().c_str());
    digitalWrite(ledPin, HIGH);
    return;
  }

  // Подключение к WiFi сети
  Serial.printf("Connecting to %s\n", settings.wifiSSID);
  String hostname = String(settings.deviceName);
  hostname.toLowerCase();
  hostname.replace("_", "-");
  WiFi.setHostname(hostname.c_str());
  
  // Быстрое отключение от текущей сети
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect();
    delay(100);
  }
  
  WiFi.mode(WIFI_STA);
  
  // Быстрые попытки подключения
  for (int attempt = 0; attempt < WIFI_RECONNECT_ATTEMPTS; attempt++) {
    WiFi.begin(settings.wifiSSID, settings.wifiPassword);
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && 
           millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT) {
      delay(100);
      Serial.print(".");
      digitalWrite(ledPin, !digitalRead(ledPin));
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("\nWiFi connected!"));
      Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("Hostname: %s\n", hostname.c_str());
      digitalWrite(ledPin, LOW);
      return;
    }
    
    // Если не удалось подключиться, пробуем снова
    WiFi.disconnect();
    delay(100);
  }
  
  // Если все попытки неудачны, запускаем AP
  Serial.println(F("\nFailed to connect to WiFi. Starting AP as fallback."));
  resetWiFi();
}

// Подключение к MQTT
bool setupMQTT() {
  if (WiFi.status() != WL_CONNECTED || strlen(settings.mqttServer) == 0) {
    Serial.println(F("MQTT setup skipped: No WiFi or MQTT server not configured."));
    return false;
  }
  Serial.printf("Attempting MQTT connection to %s:%d...\n", settings.mqttServer, settings.mqttPort);
  mqttClient.setServer(settings.mqttServer, settings.mqttPort);
  // mqttClient.setCallback(callback); // Добавьте, если нужна обработка входящих сообщений

  String clientId = String(settings.deviceName) + "-client-" + String(random(0xffff), HEX);
  Serial.printf("Connecting with Client ID: %s\n", clientId.c_str());

  bool result;
  if (strlen(settings.mqttUser) > 0) {
    result = mqttClient.connect(clientId.c_str(), settings.mqttUser, settings.mqttPassword);
  } else {
    result = mqttClient.connect(clientId.c_str());
  }

  if (result) {
    Serial.println(F("MQTT connected!"));
    // Можно подписаться на топики здесь, если нужно
    // client.subscribe("your/topic");
  } else {
    Serial.print(F("MQTT connection failed, rc="));
    Serial.print(mqttClient.state());
    Serial.println(F(" Retrying later..."));
  }
  return result;
}

// Сброс настроек WiFi
void resetWiFi() {
  Serial.println(F("Resetting WiFi settings and restarting..."));
  digitalWrite(ledPin, HIGH);

  EEPROM.begin(sizeof(Settings));
  // Читаем текущие настройки, чтобы не затереть остальные
  EEPROM.get(0, settings);

  // Очищаем только WiFi поля
  Serial.println(F("Clearing WiFi SSID and Password."));
  memset(settings.wifiSSID, 0, sizeof(settings.wifiSSID));
  memset(settings.wifiPassword, 0, sizeof(settings.wifiPassword));
  settings.settingsVersion = CURRENT_SETTINGS_VERSION; // Убедимся, что версия актуальна

  Serial.println(F("Saving settings with cleared WiFi info..."));
  EEPROM.put(0, settings);
  bool commitOK = EEPROM.commit();
  EEPROM.end();

  if (!commitOK) {
    Serial.println(F("ERROR: EEPROM commit failed during WiFi reset!"));
  }

  // Индикация и перезагрузка
  digitalWrite(ledPin, LOW); delay(200); digitalWrite(ledPin, HIGH); delay(200); digitalWrite(ledPin, LOW);
  Serial.println(F("Restarting now to apply changes (AP mode)."));
  delay(500);
  ESP.restart();
}

// Полный сброс настроек к заводским
void factoryReset() {
  Serial.println(F("!!! FACTORY RESET initiated !!!"));
  digitalWrite(ledPin, HIGH); // Индикация

  EEPROM.begin(sizeof(Settings));

  // Просто записываем пустую структуру (или структуру с дефолтами)
   Settings defaultSettings;
   memset(&defaultSettings, 0, sizeof(Settings)); // Очищаем структуру

   // Устанавливаем дефолтные значения
   defaultSettings.settingsVersion = CURRENT_SETTINGS_VERSION;
   String defaultName = getDeviceName(); // Генерируем имя заранее
   strlcpy(defaultSettings.deviceName, defaultName.c_str(), sizeof(defaultSettings.deviceName));
   strlcpy(defaultSettings.mqttServer, "", sizeof(defaultSettings.mqttServer)); // Пустой сервер по умолчанию
   defaultSettings.mqttPort = 1883;
   strlcpy(defaultSettings.mqttUser, "", sizeof(defaultSettings.mqttUser));
   strlcpy(defaultSettings.mqttPassword, "", sizeof(defaultSettings.mqttPassword));
   defaultSettings.updateInterval = DEFAULT_UPDATE_INTERVAL;
   defaultSettings.calibration.phOffset = DEFAULT_PH_OFFSET;
   defaultSettings.calibration.phScale = DEFAULT_PH_SCALE;
   defaultSettings.calibration.orpOffset = DEFAULT_ORP_OFFSET;
   defaultSettings.calibration.orpScale = DEFAULT_ORP_SCALE;
   // WiFi SSID и Password остаются пустыми

  Serial.println(F("Saving default settings to EEPROM..."));
  EEPROM.put(0, defaultSettings); // Записываем дефолты
  bool commitOK = EEPROM.commit();
  EEPROM.end();

  if (commitOK) {
    Serial.println(F("Default settings saved successfully."));
  } else {
    Serial.println(F("ERROR: EEPROM commit failed during factory reset!"));
  }

  // Индикация и перезагрузка
  for (int i = 0; i < 5; i++) { digitalWrite(ledPin, LOW); delay(100); digitalWrite(ledPin, HIGH); delay(100); }
  digitalWrite(ledPin, LOW);
  Serial.println(F("Factory reset complete. Restarting now (AP mode)."));
  delay(1000);
  ESP.restart();
}

// Генерация имени устройства
String getDeviceName() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char baseName[20];
  snprintf(baseName, sizeof(baseName), "Water_%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(baseName);
}

// --- Функции веб-интерфейса ---

// Генерация CSS стилей
String getCSS() {
  String css = "<style>";
  css += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, 'Open Sans', 'Helvetica Neue', sans-serif; margin: 0; padding: 0; background-color: #f8f9fa; color: #343a40; }";
  css += ".container { max-width: 800px; margin: 20px auto; background: #ffffff; padding: 25px; border-radius: 8px; box-shadow: 0 4px 12px rgba(0,0,0,0.08); }";
  css += "h1, h2, h3 { color: #212529; margin-top: 0; margin-bottom: 1rem; }";
  css += "nav { background-color: #e9ecef; padding: 10px 0; margin-bottom: 25px; border-radius: 5px; text-align: center;}";
  css += "nav a { text-decoration: none; color: #495057; margin: 0 10px; padding: 8px 15px; border-radius: 4px; transition: background-color 0.2s, color 0.2s; font-weight: 500; }";
  css += "nav a:hover { background-color: #dee2e6; color: #212529; }";
  css += "nav a.active { background-color: #007bff; color: #ffffff; }";
  css += "label { display: block; margin: 1rem 0 0.5rem 0; color: #495057; font-weight: 500; }";
  css += "input[type='text'], input[type='password'], input[type='number'] { width: 100%; padding: 0.75rem; margin-bottom: 0.75rem; border: 1px solid #ced4da; border-radius: 4px; font-size: 1rem; box-sizing: border-box; transition: border-color 0.2s, box-shadow 0.2s; }";
  css += "input[type='text']:focus, input[type='password']:focus, input[type='number']:focus { border-color: #80bdff; outline: 0; box-shadow: 0 0 0 0.2rem rgba(0,123,255,.25); }";
  css += "input[type='submit'], .button { background-color: #007bff; color: white; border: none; padding: 0.75rem 1.25rem; margin-top: 1rem; border-radius: 4px; cursor: pointer; font-size: 1rem; transition: background-color 0.2s; font-weight: 500; }";
  css += "input[type='submit']:hover, .button:hover { background-color: #0056b3; }";
  css += ".button.warning { background-color: #dc3545; } .button.warning:hover { background-color: #c82333; }";
  css += ".message { padding: 1rem; margin: 1.5rem 0; border-radius: 4px; border: 1px solid transparent; }";
  css += ".success { background-color: #d1e7dd; color: #0f5132; border-color: #badbcc; }";
  css += ".error { background-color: #f8d7da; color: #842029; border-color: #f5c2c7; }";
  css += ".sensor-data div, .calibration-section div { margin-bottom: 0.75rem; font-size: 1.1rem; }";
  css += ".sensor-label { font-weight: 500; color: #6c757d; min-width: 140px; display: inline-block; }";
  css += ".value-display { font-family: 'Courier New', Courier, monospace; font-weight: bold; color: #007bff; }";
  css += ".form-section { margin-bottom: 2rem; padding-bottom: 1.5rem; border-bottom: 1px solid #e9ecef; } .form-section:last-child { border-bottom: none; }";
  css += ".reset-section { margin-top: 2rem; padding: 1.5rem; border: 1px solid #f5c2c7; background-color: #f8d7da; border-radius: 5px;}";
  css += "small { color: #6c757d; font-size: 0.875em; display: block; margin-top: -0.5rem; margin-bottom: 0.5rem;}";
  css += "</style>";
  return css;
}

// Генерация навигационного меню
String getNavigationMenu(String activePage = "") {
  String menu = "<nav class='menu'>";
  menu += String("<a href='/' ") + (activePage == "root" ? "class='active'" : "") + ">Данные</a>";
  menu += String("<a href='/settings' ") + (activePage == "settings" ? "class='active'" : "") + ">Настройки</a>";
  menu += String("<a href='/calibration' ") + (activePage == "calibration" ? "class='active'" : "") + ">Калибровка</a>";
  menu += String("<a href='/update' ") + (activePage == "update" ? "class='active'" : "") + ">Обновление ПО</a>";
  menu += String("<a href='/about' ") + (activePage == "about" ? "class='active'" : "") + ">О проекте</a>";
  menu += "</nav>";
  return menu;
}

// Обработчик главной страницы (/)
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>ORP/pH Sensor - Данные</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<meta http-equiv='refresh' content='10'>";// Автообновление каждые 10 сек
  html += getCSS();
  html += "</head><body><div class='container'>";
  html += getNavigationMenu("root");
  html += "<h1>Текущие показания</h1>";
  html += "<div class='sensor-data'>";
  html += "<div><span class='sensor-label'>pH:</span> <span class='value-display'>" + String(sensorValues.phValue, 2) + "</span></div>";
  html += "<div><span class='sensor-label'>ORP:</span> <span class='value-display'>" + String(sensorValues.orpValue, 1) + "</span> mV</div>";
  html += "<hr style='margin: 1rem 0; border-top: 1px solid #eee;'>";
  html += "<div><span class='sensor-label' style='color:#adb5bd;'>pH Raw ADC:</span> <span class='value-display' style='color:#adb5bd;'>" + String(sensorValues.phRaw, 0) + "</span></div>";
  html += "<div><span class='sensor-label' style='color:#adb5bd;'>ORP Raw ADC:</span> <span class='value-display' style='color:#adb5bd;'>" + String(sensorValues.orpRaw, 0) + "</span></div>";
  html += "</div>";
  html += "</div></body></html>";
  server.sendHeader("Content-Type", "text/html; charset=UTF-8");
  server.send(200, "text/html", html);
}

// Обработчик страницы настроек (/settings)
void handleSettings() {
  String message = "";
  bool needRestart = false;

  if (server.method() == HTTP_POST) {
    bool settingsUpdated = false;
    Serial.println(F("Processing POST /settings"));

    // --- Имя устройства ---
    if (server.hasArg("deviceName")) {
      String newName = server.arg("deviceName");
      newName.trim();
      if (newName.length() > 0 && newName.length() < sizeof(settings.deviceName)) {
         bool nameChanged = (strcmp(settings.deviceName, newName.c_str()) != 0);
         if (nameChanged) {
            Serial.printf("Device name changing from '%s' to '%s'\n", settings.deviceName, newName.c_str());
            strlcpy(settings.deviceName, newName.c_str(), sizeof(settings.deviceName));
            settingsUpdated = true;
            needRestart = true;
         }
      } else {
          Serial.printf("Invalid device name received: '%s'\n", newName.c_str());
          message += "Ошибка: Недопустимое имя устройства.<br>";
      }
    }

    // --- WiFi ---
    if (server.hasArg("wifiSSID")) {
      String newSSID = server.arg("wifiSSID");
      String newPassword = server.hasArg("wifiPassword") ? server.arg("wifiPassword") : "";
      if (strcmp(settings.wifiSSID, newSSID.c_str()) != 0 || strcmp(settings.wifiPassword, newPassword.c_str()) != 0) {
        Serial.printf("WiFi settings changing. New SSID: '%s'\n", newSSID.c_str());
        strlcpy(settings.wifiSSID, newSSID.c_str(), sizeof(settings.wifiSSID));
        strlcpy(settings.wifiPassword, newPassword.c_str(), sizeof(settings.wifiPassword));
        settingsUpdated = true;
        needRestart = true;
      }
    }

    // --- MQTT ---
    bool mqttChanged = false;
    if (server.hasArg("mqttServer")) {
      String newServer = server.arg("mqttServer");
      if (strcmp(settings.mqttServer, newServer.c_str()) != 0) {
        strlcpy(settings.mqttServer, newServer.c_str(), sizeof(settings.mqttServer));
        mqttChanged = true;
      }
    }
    if (server.hasArg("mqttPort")) {
      int newPort = server.arg("mqttPort").toInt();
      if (newPort > 0 && newPort <= 65535 && settings.mqttPort != newPort) {
        settings.mqttPort = newPort;
        mqttChanged = true;
      }
    }
     if (server.hasArg("mqttUser")) {
        String newUser = server.arg("mqttUser");
        if (strcmp(settings.mqttUser, newUser.c_str()) != 0) {
           strlcpy(settings.mqttUser, newUser.c_str(), sizeof(settings.mqttUser));
           mqttChanged = true;
        }
     }
     if (server.hasArg("mqttPassword")) {
        String newMqttPwd = server.arg("mqttPassword");
         if (strcmp(settings.mqttPassword, newMqttPwd.c_str()) != 0) {
             strlcpy(settings.mqttPassword, newMqttPwd.c_str(), sizeof(settings.mqttPassword));
             mqttChanged = true;
         }
     }
     if (mqttChanged) {
        Serial.println(F("MQTT settings changed. Will reconnect."));
        settingsUpdated = true;
        // Переподключение произойдет в loop()
     }

    // --- Интервал обновления ---
    if (server.hasArg("updateInterval")) {
      int newInterval = server.arg("updateInterval").toInt();
      if (newInterval >= 1000 && newInterval <= 3600000 && settings.updateInterval != newInterval) {
        Serial.printf("Update interval changing from %d to %d\n", settings.updateInterval, newInterval);
        settings.updateInterval = newInterval;
        settingsUpdated = true;
      }
    }

    // --- Сохранение ---
    if (settingsUpdated) {
      saveSettings();
      message += "Настройки успешно сохранены.";
      if (needRestart) {
        message += " Устройство перезагрузится через 3 секунды для применения изменений.";
      }
    } else if (message.length() == 0) {
      message = "Изменений не было.";
    }
  } // end POST

  // --- Отображение страницы (GET или после POST) ---
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>ORP/pH Sensor - Настройки</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += getCSS();
  html += "</head><body><div class='container'>";
  html += getNavigationMenu("settings");
  html += "<h1>Настройки устройства</h1>";

  if (message.length() > 0) {
    String messageClass = message.indexOf("Ошибка") != -1 ? "error" : "success";
    html += String("<div class='message ") + messageClass + "'>" + message + "</div>";
     if (needRestart) {
        html += "<script>setTimeout(function(){ window.location.href='/'; }, 3000);</script>";
     } else if (server.method() == HTTP_POST && message.indexOf("Ошибка") == -1) {
         html += "<script>setTimeout(function(){ window.location.href='/settings'; }, 1000);</script>";
     }
  }

  html += "<form method='post'>";
  html += "<div class='form-section'>";
  html += "<h3>Общие</h3>";
  html += "<label for='deviceName'>Имя устройства:</label>";
  html += "<input type='text' id='deviceName' name='deviceName' value='" + String(settings.deviceName) + "' required maxlength='" + String(sizeof(settings.deviceName)-1) + "'>";
  html += "<small>Используется для Hostname, MQTT Client ID, имени точки доступа.</small>";
  html += "<label for='updateInterval'>Интервал обновления (мс):</label>";
  html += "<input type='number' id='updateInterval' name='updateInterval' value='" + String(settings.updateInterval) + "' min='1000' max='3600000' required>";
  html += "<small>Как часто считывать показания датчиков (1000 мс = 1 сек).</small>";
  html += "</div>";

  html += "<div class='form-section'>";
  html += "<h3>WiFi</h3>";
  html += "<label for='wifiSSID'>Имя сети (SSID):</label>";
  html += "<input type='text' id='wifiSSID' name='wifiSSID' value='" + String(settings.wifiSSID) + "' maxlength='" + String(sizeof(settings.wifiSSID)-1) + "'>";
  html += "<small>Оставьте пустым для запуска устройства в режиме точки доступа.</small>";
  html += "<label for='wifiPassword'>Пароль:</label>";
  html += "<input type='password' id='wifiPassword' name='wifiPassword' value='" + String(settings.wifiPassword) + "' maxlength='" + String(sizeof(settings.wifiPassword)-1) + "'>";
  html += "</div>";

  html += "<div class='form-section'>";
  html += "<h3>MQTT</h3>";
  html += "<label for='mqttServer'>Сервер:</label>";
  html += "<input type='text' id='mqttServer' name='mqttServer' value='" + String(settings.mqttServer) + "' maxlength='" + String(sizeof(settings.mqttServer)-1) + "' placeholder='Например: mqtt.local или 192.168.1.100'>";
  html += "<small>IP-адрес или доменное имя MQTT сервера. Оставьте пустым для отключения MQTT.</small>";
  html += "<label for='mqttPort'>Порт:</label>";
  html += "<input type='number' id='mqttPort' name='mqttPort' value='" + String(settings.mqttPort) + "' min='1' max='65535' required placeholder='Обычно 1883'>";
  html += "<small>Порт MQTT сервера. По умолчанию: 1883</small>";
  html += "<label for='mqttUser'>Пользователь (если требуется):</label>";
  html += "<input type='text' id='mqttUser' name='mqttUser' value='" + String(settings.mqttUser) + "' maxlength='" + String(sizeof(settings.mqttUser)-1) + "' placeholder='Имя пользователя MQTT'>";
  html += "<label for='mqttPassword'>Пароль (если требуется):</label>";
  html += "<input type='password' id='mqttPassword' name='mqttPassword' value='" + String(settings.mqttPassword) + "' maxlength='" + String(sizeof(settings.mqttPassword)-1) + "' placeholder='Пароль MQTT'>";
  html += "</div>";

  html += "<input type='submit' value='Сохранить настройки'>";
  html += "</form>";

  html += "</div></body></html>";
  server.sendHeader("Content-Type", "text/html; charset=UTF-8");
  server.send(200, "text/html", html);

  // Перезагрузка после отправки ответа, если нужно
  if (needRestart && server.method() == HTTP_POST) {
    Serial.println(F("Restarting ESP due to settings change..."));
    delay(3000);
    ESP.restart();
  }
}

// Обработчик страницы калибровки (/calibration)
void handleCalibration() {
    String message = "";
    bool calibrationUpdated = false;

    if (server.method() == HTTP_POST) {
        Serial.println(F("Processing POST /calibration"));
        if (server.hasArg("reset_calibration")) {
             Serial.println(F("Resetting calibration to defaults..."));
             settings.calibration.phOffset = DEFAULT_PH_OFFSET;
             settings.calibration.phScale = DEFAULT_PH_SCALE;
             settings.calibration.orpOffset = DEFAULT_ORP_OFFSET;
             settings.calibration.orpScale = DEFAULT_ORP_SCALE;
             saveSettings();
             message = "Калибровка сброшена на значения по умолчанию.";
             calibrationUpdated = true;
        } else {
            bool updateOk = true;
            float tempPhOffset = settings.calibration.phOffset;
            float tempPhScale = settings.calibration.phScale;
            float tempOrpOffset = settings.calibration.orpOffset;
            float tempOrpScale = settings.calibration.orpScale;

            if (server.hasArg("phOffset")) tempPhOffset = server.arg("phOffset").toFloat();
            if (server.hasArg("phScale")) tempPhScale = server.arg("phScale").toFloat();
            if (server.hasArg("orpOffset")) tempOrpOffset = server.arg("orpOffset").toFloat();
            if (server.hasArg("orpScale")) tempOrpScale = server.arg("orpScale").toFloat();

            if (isnan(tempPhOffset) || isinf(tempPhOffset) ||
                isnan(tempPhScale) || isinf(tempPhScale) || tempPhScale == 0 ||
                isnan(tempOrpOffset) || isinf(tempOrpOffset) ||
                isnan(tempOrpScale) || isinf(tempOrpScale) || tempOrpScale == 0)
            {
                Serial.println(F("Invalid calibration values received."));
                message = "Ошибка: Недопустимые значения калибровки.";
                updateOk = false;
            }

            if (updateOk) {
                settings.calibration.phOffset = tempPhOffset;
                settings.calibration.phScale = tempPhScale;
                settings.calibration.orpOffset = tempOrpOffset;
                settings.calibration.orpScale = tempOrpScale;
                saveSettings();
                message = "Калибровка успешно сохранена.";
                calibrationUpdated = true;
            }
        }
    } // end POST

    // --- Отображение страницы (GET или после POST) ---
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<title>ORP/pH Sensor - Калибровка</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += getCSS();
     html += "<script>";
     html += "function confirmReset() { return confirm('Вы уверены, что хотите сбросить калибровку на значения по умолчанию?'); }";
     html += "</script>";
    html += "</head><body><div class='container'>";
    html += getNavigationMenu("calibration");
    html += "<h1>Калибровка датчиков</h1>";

    if (message.length() > 0) {
        String messageClass = message.indexOf("Ошибка") != -1 ? "error" : "success";
        html += String("<div class='message ") + messageClass + "'>" + message + "</div>";
        if (calibrationUpdated && message.indexOf("Ошибка") == -1) {
             html += "<script>setTimeout(function(){ window.location.href='/calibration'; }, 1000);</script>";
        }
    }

    html += "<h3>Текущие показания (для справки)</h3>";
    html += "<div class='sensor-data'>";
    html += "<div><span class='sensor-label'>pH:</span> <span class='value-display'>" + String(sensorValues.phValue, 2) + "</span></div>";
    html += "<div><span class='sensor-label'>ORP:</span> <span class='value-display'>" + String(sensorValues.orpValue, 1) + "</span> mV</div>";
    html += "</div>";

    html += "<h3>Настройка калибровки</h3>";
    html += "<p>Отрегулируйте смещение (Offset) и множитель (Scale) для каждого датчика, чтобы показания соответствовали эталонным значениям. Формула: `Итоговое_Значение = (Сырое_Значение_в_ед_изм - Offset) * Scale`</p>";
    html += "<form method='post'>";
    html += "<div class='form-section'>";
    html += "<h4>pH Датчик</h4>";
    html += "<label for='phOffset'>Смещение pH (Offset):</label>";
    html += "<input type='number' step='0.01' id='phOffset' name='phOffset' value='" + String(settings.calibration.phOffset, 2) + "'>";
     html += "<small>Вычитается из рассчитанного значения pH перед умножением.</small>";
    html += "<label for='phScale'>Множитель pH (Scale):</label>";
    html += "<input type='number' step='0.01' id='phScale' name='phScale' value='" + String(settings.calibration.phScale, 2) + "'>";
     html += "<small>Коэффициент, на который умножается значение после вычета смещения.</small>";
    html += "</div>";

    html += "<div class='form-section'>";
    html += "<h4>ORP Датчик</h4>";
    html += "<label for='orpOffset'>Смещение ORP (Offset, mV):</label>";
    html += "<input type='number' step='0.1' id='orpOffset' name='orpOffset' value='" + String(settings.calibration.orpOffset, 1) + "'>";
     html += "<small>Вычитается из рассчитанного значения ORP (в мВ) перед умножением.</small>";
    html += "<label for='orpScale'>Множитель ORP (Scale):</label>";
    html += "<input type='number' step='0.01' id='orpScale' name='orpScale' value='" + String(settings.calibration.orpScale, 2) + "'>";
     html += "<small>Коэффициент, на который умножается значение после вычета смещения.</small>";
    html += "</div>";

    html += "<input type='submit' value='Сохранить калибровку'>";
    html += "</form>";

    html += "<div class='reset-section'>";
    html += "<h3>Сброс калибровки</h3>";
    html += "<form method='post' onsubmit='return confirmReset();'>";
    html += "<input type='hidden' name='reset_calibration' value='true'>";
    html += "<input type='submit' class='button warning' value='Сбросить на по умолчанию'>";
    html += "</form>";
    html += "</div>";

    html += "</div></body></html>";
    server.sendHeader("Content-Type", "text/html; charset=UTF-8");
    server.send(200, "text/html", html);
}

// Страница обновления прошивки
void handleUpdateFirmware() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<title>ORP/pH Sensor - Обновление ПО</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += getCSS();
    html += "</head><body><div class='container'>";
    html += getNavigationMenu("update");
    html += "<h1>Обновление прошивки</h1>";
    html += "<p>Выберите файл прошивки (.bin) для загрузки на устройство.</p>";
    html += "<form method='POST' action='/doUpdate' enctype='multipart/form-data'>";
    html += "<label for='update'>Файл прошивки:</label>";
    html += "<input type='file' id='update' name='update' accept='.bin' required style='padding: 0.5rem; border: 1px solid #ccc; border-radius: 4px; display: block; margin-bottom: 1rem;'><br>";
    html += "<input type='submit' value='Загрузить и обновить'>";
    html += "</form>";
    html += "<p><strong>Внимание:</strong> Устройство перезагрузится после успешного обновления.</p>";
    html += "</div></body></html>";
    server.sendHeader("Content-Type", "text/html; charset=UTF-8");
    server.send(200, "text/html", html);
}

// Обработчик ответа после загрузки файла прошивки
void handleDoUpdate() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<title>ORP/pH Sensor - Результат обновления</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += getCSS();
    html += "<meta http-equiv='refresh' content='15;url=/' />"; // Увеличиваем время ожидания до 15 секунд
    html += "</head><body><div class='container'>";
    html += "<h1>Результат обновления</h1>";
    
    if (Update.hasError()) {
        html += "<div class='message error'>Ошибка обновления! Код: " + String(Update.getError()) + "</div>";
    } else {
        html += "<div class='message success'>Обновление успешно завершено! Устройство перезагружается...</div>";
        // Сохраняем настройки перед перезагрузкой
        saveSettings();
        // Даем время на сохранение
        delay(2000);
    }
    
    html += "<p><a href='/'>Вернуться на главную страницу</a></p>";
    html += "</div></body></html>";
    server.sendHeader("Content-Type", "text/html; charset=UTF-8");
    server.send(200, "text/html", html);
    
    if (!Update.hasError()) {
        // Даем время на отправку ответа клиенту
        delay(3000);
        ESP.restart();
    }
}


// Обработчик страницы "О проекте" (/about)
void handleAbout() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>ORP/pH Sensor - О проекте</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += getCSS();
  html += "</head><body><div class='container'>";
  html += getNavigationMenu("about");
  html += "<h1>О проекте</h1>";
  html += "<p>Система мониторинга pH и ORP на базе ESP32.</p>";
  
  html += "<div class='form-section'>";
  html += "<h3>Информация об устройстве</h3>";
  String hostname = String(settings.deviceName);
  hostname.toLowerCase();
  hostname.replace("_", "-");
  html += "<p><strong>Имя устройства:</strong> " + String(settings.deviceName) + "</p>";
  html += "<p><strong>Hostname (mDNS):</strong> " + hostname + ".local</p>";
  html += "<p><strong>IP-адрес:</strong> " + (WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</p>";
  html += "<p><strong>MAC-адрес:</strong> " + WiFi.macAddress() + "</p>";
  html += "<p><strong>Версия прошивки:</strong> " + String(VERSION_STRING) + " (" + String(__DATE__) + " " + String(__TIME__) + ")</p>";
  html += "</div>";

  html += "<div class='form-section'>";
  html += "<h3>Контактная информация</h3>";
  html += "<p><strong>Автор:</strong> Колесник Станислав</p>";
  html += "<p><strong>Telegram:</strong> <a href='https://t.me/Gfermoto' target='_blank'>@Gfermoto</a></p>";
  html += "<p><strong>GitHub:</strong> <a href='https://github.com/Gfermoto' target='_blank'>github.com/Gfermoto</a></p>";
  html += "</div>";

  html += "</div></body></html>";
  server.sendHeader("Content-Type", "text/html; charset=UTF-8");
  server.send(200, "text/html", html);
}

// Обработчик 404
void handleNotFound() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>Ошибка 404</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += getCSS();
  html += "</head><body><div class='container'>";
  html += getNavigationMenu();
  html += "<h1>Ошибка 404</h1>";
  html += "<p>Запрошенная страница не найдена.</p>";
  html += "<p>URI: " + server.uri() + "</p>";
  html += "</div></body></html>";
  server.sendHeader("Content-Type", "text/html; charset=UTF-8");
  server.send(404, "text/html", html);
}


// --- Функции датчиков и MQTT ---

// Чтение датчиков
void readSensors() {
    // Чтение pH
    long phSum = 0;
    for(int i=0; i<ARRAY_LENGTH; i++) {
        phArray[phArrayIndex] = analogRead(phPin);
        phSum += phArray[phArrayIndex];
        phArrayIndex = (phArrayIndex + 1) % ARRAY_LENGTH;
        delay(1);
    }
    float phAvg = (float)phSum / ARRAY_LENGTH;
    sensorValues.phRaw = phAvg;

    // !! ВАЖНО: Адаптируйте формулу под ваш модуль pH !!
    float phVoltage = phAvg * (VCC / 4095.0);
    // Пример: sensorValues.phValue = 7.0 - (phVoltage - 2.5) / 0.059;
    // Используем простую линейную для примера, ЗАМЕНИТЕ:
     sensorValues.phValue = map(phAvg, 0, 4095, 0, 14);
    // Применяем калибровку
     sensorValues.phValue = (sensorValues.phValue - settings.calibration.phOffset) * settings.calibration.phScale;

    // Чтение ORP
    long orpSum = 0;
    for(int i=0; i<ARRAY_LENGTH; i++) {
        orpArray[orpArrayIndex] = analogRead(orpPin);
        orpSum += orpArray[orpArrayIndex];
        orpArrayIndex = (orpArrayIndex + 1) % ARRAY_LENGTH;
        delay(1);
    }
     float orpAvg = (float)orpSum / ARRAY_LENGTH;
     sensorValues.orpRaw = orpAvg;

    // !! ВАЖНО: Адаптируйте формулу под ваш модуль ORP !!
    // Пример: float orpVoltage = orpAvg * (VCC / 4095.0); sensorValues.orpValue = (orpVoltage - 1.5) * 1000.0;
    // Используем простую линейную для примера, ЗАМЕНИТЕ:
     sensorValues.orpValue = (orpAvg - 2048.0) * (VCC / 4095.0) * 1000.0;
    // Применяем калибровку
     sensorValues.orpValue = (sensorValues.orpValue - settings.calibration.orpOffset) * settings.calibration.orpScale;

    Serial.printf("Readings: pH Raw: %.0f, pH: %.2f | ORP Raw: %.0f, ORP: %.1f mV\n",
                  sensorValues.phRaw, sensorValues.phValue,
                  sensorValues.orpRaw, sensorValues.orpValue);
}

// Публикация данных в MQTT
void publishSensorData() {
  if (!mqttClient.connected()) {
    // Serial.println(F("MQTT not connected, skipping publish.")); // Можно раскомментировать для отладки
    return;
  }

  StaticJsonDocument<128> doc;
  doc["ph"] = round(sensorValues.phValue * 100.0) / 100.0;
  doc["orp"] = round(sensorValues.orpValue * 10.0) / 10.0;

  String topic = "sensor/" + String(settings.deviceName) + "/state";
  char buffer[128];
  size_t len = serializeJson(doc, buffer);

  // Serial.printf("Publishing to MQTT topic: %s\n", topic.c_str()); // Можно раскомментировать для отладки
  if (!mqttClient.publish(topic.c_str(), buffer, len)) {
      Serial.println(F("MQTT publish FAILED"));
  }
} 