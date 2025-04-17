#include <WiFi.h>        // Библиотека для работы с WiFi
#include <WebServer.h>   // Веб-сервер
#include <Preferences.h>  // Хранение настроек в NVS
#include <DNSServer.h>    // DNS-сервер для режима точки доступа
#include <ESPmDNS.h>     // Для OTA
#include <WiFiUdp.h>     // Для OTA
#include <ArduinoOTA.h>  // Для OTA
#include <PubSubClient.h> // Для MQTT

// Константы и параметры
#define BOOT_BUTTON 0   // GPIO0 для кнопки BOOT
#define LED_PIN     2   // Светодиод индикации (GPIO2)
#define DNS_PORT    53  // Порт DNS сервера
#define DEBUG_MODE false // Режим отладки
const char* DEVICE_PREFIX = "ORP_pH_"; // Префикс имени устройства
const char* VERSION = "v1.2.1a"; // Версия прошивки

// Объекты для работы
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Переменные для хранения настроек
String deviceName = "";  // Имя устройства
String ssid = "";         // Имя WiFi сети
String password = "";     // Пароль WiFi сети
String mqtt_server = "";  // Адрес MQTT сервера
String mqtt_port = "1883"; // Порт MQTT сервера
String mqtt_user = "";    // Имя пользователя MQTT
String mqtt_password = ""; // Пароль MQTT
String mqtt_prefix = "homeassistant"; // Базовый префикс для MQTT топиков
bool mqtt_enabled = false; // Включен ли MQTT
String mqtt_client_id = ""; // ID клиента MQTT (будет установлен как имя устройства)

// Режим работы: true - точка доступа, false - клиент
bool ap_mode = true;

// Переменные состояния
bool ledState = false;          // Текущее состояние светодиода
bool isConnected = false;       // Флаг подключения к сети или клиента к AP
bool isResetting = false;       // Флаг процесса сброса
unsigned long ledLastToggle = 0; // Время последнего переключения светодиода

// Добавляем переменные для watchdog
static unsigned long lastReset = 0;
const unsigned long WATCHDOG_TIMEOUT = 30000; // 30 секунд

// Переменные для хранения значений сенсоров
float orpValue = 0.0;  // Значение ORP в mV
float phValue = 0.0;   // Значение pH

// Объявление функции debugPrint
void debugPrint(const String& message);

// В начале файла, после объявления mqttClient
#define MQTT_MAX_PACKET_SIZE 512  // Увеличиваем размер буфера MQTT

// Функция для безопасного вывода в Serial
void safePrintln(const String& message) {
  static bool serialInitialized = false;
  
  if (!serialInitialized) {
    Serial.begin(115200);
    delay(1000);
    Serial.flush();
    serialInitialized = true;
  }
  
  if (Serial) {
    Serial.println(message);
    delay(10); // Даем время на отправку
  }
}

void safePrint(const String& message) {
  static bool serialInitialized = false;
  
  if (!serialInitialized) {
    Serial.begin(115200);
    delay(1000);
    Serial.flush();
    serialInitialized = true;
  }
  
  if (Serial) {
    Serial.print(message);
    delay(10); // Даем время на отправку
  }
}

// Функция для формирования MQTT топиков
String getMqttTopic(const String& type, const String& name = "") {
  String topic = mqtt_prefix;
  if (type == "status") {
    topic += "/status";
  } else if (type == "sensor") {
    topic += "/sensor/orp_ph";
    if (name != "") {
      topic += "/" + name;
    }
  }
  return topic;
}

// Функция для подключения к MQTT
bool connectToMQTT() {
  if (!mqtt_enabled) {
    safePrintln("MQTT: Not enabled");
    return false;
  }
  
  safePrintln("\n=== Connecting to MQTT ===");
  safePrintln("Server: " + mqtt_server);
  safePrintln("Port:   " + mqtt_port);
  safePrintln("Client: " + mqtt_client_id);
  safePrintln("=======================\n");
  
  // Устанавливаем LWT с правильной структурой топика
  String lwtTopic = mqtt_prefix + "/sensor/" + mqtt_client_id + "/availability";
  
  if (mqttClient.connect(mqtt_client_id.c_str(), mqtt_user.c_str(), mqtt_password.c_str(), lwtTopic.c_str(), 1, true, "offline")) {
    safePrintln("MQTT: Connected successfully");
    
    // Отправляем статус online
    mqttClient.publish(lwtTopic.c_str(), "online", true);
    
    // После успешного подключения отправляем конфигурацию
    sendMqttData();
    return true;
  } else {
    safePrintln("MQTT: Connection failed");
    safePrintln("State: " + String(mqttClient.state()));
    return false;
  }
}

// Функция для проверки состояния MQTT
String getMqttStatus() {
  static String lastStatus = "";
  String currentStatus;
  
  if (!mqtt_enabled) {
    currentStatus = "disabled";
  } else if (mqttClient.connected()) {
    currentStatus = "connected";
  } else {
    // Если не подключен, пробуем подключиться
    connectToMQTT();
    
    int state = mqttClient.state();
    if (state != MQTT_CONNECTION_TIMEOUT && 
        state != MQTT_CONNECTION_LOST && 
        state != MQTT_CONNECT_FAILED) {
      currentStatus = "connecting";
    } else {
      currentStatus = "error";
    }
  }
  
  // Выводим сообщение только если статус изменился
  if (currentStatus != lastStatus) {
    Serial.print("MQTT Status: ");
    Serial.println(currentStatus);
    lastStatus = currentStatus;
  }
  
  return currentStatus;
}

// Обработка страницы конфигурации
void handleRoot() {
  server.send(200, "text/html", getAPConfigPage());
}

// Улучшенная функция сохранения настроек
void saveSettings() {
  Serial.println("Saving settings to NVS...");
  
  // Сохраняем WiFi настройки
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putBool("ap_mode", ap_mode);
  preferences.end();
  
  // Сохраняем MQTT настройки
  preferences.begin("mqtt", false);
  preferences.clear();
  preferences.putBool("enabled", mqtt_enabled);
  preferences.putString("server", mqtt_server);
  preferences.putString("port", mqtt_port);
  preferences.putString("user", mqtt_user);
  preferences.putString("password", mqtt_password);
  preferences.putString("prefix", mqtt_prefix);
  preferences.end();
  
  Serial.println("Settings saved successfully");
}

// Страница конфигурации точки доступа и WiFi
String getAPConfigPage() {
  // Формируем HTML-страницу с настройками
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<meta http-equiv='Cache-Control' content='no-cache, no-store, must-revalidate'>";
  html += "<meta http-equiv='Pragma' content='no-cache'>";
  html += "<meta http-equiv='Expires' content='0'>";
  html += "<title>Eyera " + deviceName + " sensor</title>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;margin:0;padding:0;background-color:#121212;color:#e0e0e0;}";
  html += ".container{max-width:800px;margin:20px auto;padding:20px;background-color:#1e1e1e;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.3);}";
  html += "h1{color:#90caf9;margin-bottom:20px;font-size:24px;font-weight:500;}";
  html += "h2{color:#bb86fc;margin-top:30px;margin-bottom:15px;font-size:18px;font-weight:500;border-bottom:1px solid #333;padding-bottom:10px;}";
  html += ".section{margin-bottom:30px;padding-bottom:20px;border-bottom:1px solid #333;}";
  html += ".section:last-child{border-bottom:none;}";
  html += "label{display:block;margin:10px 0 5px;color:#b0b0b0;font-size:14px;}";
  html += "input[type=text],input[type=password],input[type=number]{width:100%;padding:10px;margin:5px 0;border:1px solid #333;border-radius:4px;box-sizing:border-box;font-size:14px;background-color:#2d2d2d;color:#e0e0e0;}";
  html += "input[type=text]:focus,input[type=password]:focus,input[type=number]:focus{border-color:#90caf9;outline:none;}";
  html += "input[type=checkbox]{margin-right:10px;vertical-align:middle;}";
  html += "input[type=submit]{background-color:#90caf9;color:#121212;border:none;padding:12px 20px;border-radius:4px;cursor:pointer;font-size:14px;margin-top:15px;transition:background-color 0.3s;}";
  html += "input[type=submit]:hover{background-color:#64b5f6;}";
  html += "input[type=submit].warning{background-color:#f44336;}";
  html += "input[type=submit].warning:hover{background-color:#d32f2f;}";
  html += ".checkbox-label{display:inline-block;margin:10px 0;color:#b0b0b0;font-size:14px;}";
  html += ".info-text{color:#888;font-size:13px;margin-top:5px;}";
  html += ".header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px;}";
  html += ".header h1{margin:0;}";
  html += ".header .version{color:#888;font-size:14px;}";
  html += ".ota-link{display:inline-block;margin-top:15px;color:#90caf9;text-decoration:none;font-size:14px;}";
  html += ".ota-link:hover{text-decoration:underline;}";
  html += ".file-input{width:100%;padding:10px;margin:5px 0;border:1px solid #333;border-radius:4px;box-sizing:border-box;font-size:14px;background-color:#2d2d2d;color:#e0e0e0;}";
  html += ".progress-bar{width:100%;height:20px;background-color:#2d2d2d;border-radius:4px;margin-top:10px;overflow:hidden;display:none;}";
  html += ".progress-bar-fill{height:100%;background-color:#90caf9;width:0%;transition:width 0.3s;}";
  html += ".mqtt-status { margin: 10px 0; display: flex; align-items: center; }";
  html += ".status-indicator { width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }";
  html += ".status-indicator.disabled { background-color: #000000; }";
  html += ".status-indicator.connecting { background-color: #ffa500; }";
  html += ".status-indicator.connected { background-color: #4CAF50; }";
  html += ".status-indicator.error { background-color: #f44336; }";
  html += ".status-text { color: #b0b0b0; font-size: 14px; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<div class='header'>";
  html += "<h1>Eyera " + deviceName + " sensor</h1>";
  html += "<span class='version'>" + String(VERSION) + "</span>";
  html += "</div>";
  
  // Секция статуса
  html += "<div class='section'>";
  html += "<h2>Device Status</h2>";
  html += "<div class='status'>";
  html += "<div class='status-item'><span class='status-label'>Mode:</span><span class='status-value'>" + String(ap_mode ? "Access Point" : "WiFi Client") + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Connection:</span><span class='status-value " + String(isConnected ? "connected" : "disconnected") + "'>" + String(isConnected ? "Connected" : "Disconnected") + "</span></div>";
  if (!ap_mode) {
    html += "<div class='status-item'><span class='status-label'>IP Address:</span><span class='status-value'>" + WiFi.localIP().toString() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Signal Strength:</span><span class='status-value'>" + String(WiFi.RSSI()) + " dBm</span></div>";
  } else {
    html += "<div class='status-item'><span class='status-label'>AP IP Address:</span><span class='status-value'>" + WiFi.softAPIP().toString() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Connected Clients:</span><span class='status-value'>" + String(WiFi.softAPgetStationNum()) + "</span></div>";
  }
  html += "</div>";
  html += "</div>";
  
  // Секция настроек WiFi
  html += "<div class='section'>";
  html += "<h2>WiFi Settings</h2>";
  html += "<form action='/save-wifi' method='POST'>";
  html += "<label for='ssid'>WiFi Name (SSID):</label>";
  html += "<input type='text' name='ssid' value='" + ssid + "' required>";
  html += "<label for='password'>WiFi Password:</label>";
  html += "<input type='password' name='password' value='" + password + "'>";
  html += "<input type='submit' value='Save WiFi Settings'>";
  html += "</form>";
  html += "</div>";
  
  // Секция настроек MQTT - показываем только в режиме клиента
  if (!ap_mode) {
    html += "<div class='section'>";
    html += "<h2>MQTT Settings</h2>";
    html += "<form action='/save-mqtt' method='POST'>";
    html += "<label class='checkbox-label'><input type='checkbox' name='mqtt_enabled' " + String(mqtt_enabled ? "checked" : "") + "> Enable MQTT</label>";
    html += "<div class='mqtt-status' id='mqttStatus'>";
    html += "<span class='status-indicator " + getMqttStatus() + "'></span>";
    html += "<span class='status-text'>" + getMqttStatus() + "</span>";
    html += "</div>";
    html += "<label for='mqtt_server'>MQTT Server:</label>";
    html += "<input type='text' name='mqtt_server' value='" + mqtt_server + "'>";
    html += "<label for='mqtt_port'>MQTT Port:</label>";
    html += "<input type='number' name='mqtt_port' value='" + mqtt_port + "'>";
    html += "<label for='mqtt_user'>MQTT Username:</label>";
    html += "<input type='text' name='mqtt_user' value='" + mqtt_user + "'>";
    html += "<label for='mqtt_password'>MQTT Password:</label>";
    html += "<input type='password' name='mqtt_password' value='" + mqtt_password + "'>";
    html += "<label for='mqtt_prefix'>MQTT Prefix:</label>";
    html += "<input type='text' name='mqtt_prefix' value='" + mqtt_prefix + "'>";
    html += "<input type='submit' value='Save MQTT Settings'>";
    html += "</form>";
    html += "</div>";
    
    // Добавляем стили для индикатора
    html += "<style>";
    html += ".mqtt-status { margin: 10px 0; display: flex; align-items: center; }";
    html += ".status-indicator { width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }";
    html += ".status-indicator.disabled { background-color: #000000; }";
    html += ".status-indicator.connecting { background-color: #ffa500; }";
    html += ".status-indicator.connected { background-color: #4CAF50; }";
    html += ".status-indicator.error { background-color: #f44336; }";
    html += ".status-text { color: #b0b0b0; font-size: 14px; }";
    html += "</style>";
    
    // Добавляем JavaScript для обновления статуса
    html += "<script>";
    html += "function updateMqttStatus() {";
    html += "  fetch('/mqtt-status').then(response => response.json())";
    html += "    .then(data => {";
    html += "      const indicator = document.querySelector('.status-indicator');";
    html += "      const text = document.querySelector('.status-text');";
    html += "      indicator.className = 'status-indicator ' + data.status;";
    html += "      text.textContent = data.status;";
    html += "    });";
    html += "}";
    html += "setInterval(updateMqttStatus, 5000);"; // Обновляем каждые 5 секунд
    html += "</script>";
  }
  
  // Добавляем кнопку сброса настроек только в режиме клиента
  if (!ap_mode) {
    html += "<div class='section'>";
    html += "<h2>Reset WiFi Settings</h2>";
    html += "<form action='/reset-wifi' method='POST'>";
    html += "<input type='submit' class='warning' value='Reset WiFi Settings'>";
    html += "<p class='info-text'>This will erase all WiFi settings and restart the device in Access Point mode.</p>";
    html += "</form>";
    html += "</div>";
  }
  
  // Секция OTA обновления (только в режиме клиента)
  if (!ap_mode) {
    html += "<div class='section'>";
    html += "<h2>Firmware Update</h2>";
    html += "<form action='/update' method='POST' enctype='multipart/form-data'>";
    html += "<label for='update'>Select firmware file:</label>";
    html += "<input type='file' name='update' class='file-input' accept='.bin'>";
    html += "<div class='progress-bar' id='progress-bar'>";
    html += "<div class='progress-bar-fill' id='progress-bar-fill'></div>";
    html += "</div>";
    html += "<input type='submit' value='Update Firmware'>";
    html += "</form>";
    html += "<script>";
    html += "document.querySelector('form').addEventListener('submit', function(e) {";
    html += "  var fileInput = document.querySelector('input[type=file]');";
    html += "  if (fileInput.files.length === 0) {";
    html += "    e.preventDefault();";
    html += "    alert('Please select a firmware file');";
    html += "    return;";
    html += "  }";
    html += "  var progressBar = document.getElementById('progress-bar');";
    html += "  var progressFill = document.getElementById('progress-bar-fill');";
    html += "  progressBar.style.display = 'block';";
    html += "  var xhr = new XMLHttpRequest();";
    html += "  xhr.upload.addEventListener('progress', function(e) {";
    html += "    if (e.lengthComputable) {";
    html += "      var percent = Math.round((e.loaded / e.total) * 100);";
    html += "      progressFill.style.width = percent + '%';";
    html += "    }";
    html += "  });";
    html += "});";
    html += "</script>";
    html += "</div>";
  }
  
  html += "</div></body></html>";
  return html;
}

// Обработчики для сохранения настроек
void handleSaveWifi() {
  // Проверяем метод запроса
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  
  // Получаем значения из формы
  ssid = server.arg("ssid");
  password = server.arg("password");
  
  Serial.println("Saving WiFi settings:");
  Serial.println("SSID: " + ssid);
  Serial.println(String("Password: ") + (password.length() > 0 ? "********" : "not set"));
  
  // Сохраняем в энергонезависимую память
  preferences.begin("wifi", false);
  preferences.clear(); // Очищаем старые настройки
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putBool("ap_mode", false); // Сохраняем режим клиента
  preferences.end();
  
  Serial.println("Settings saved to NVS");
  
  // Отправляем ответ пользователю
  server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='5;url=/'></head><body><h1>WiFi settings saved!</h1><p>The device will try to connect to the WiFi network.</p><p>If connection fails, the Access Point will be restarted.</p><p>Redirecting in 5 seconds...</p></body></html>");
  
  // Переходим в режим клиента
  ap_mode = false;
  WiFi.disconnect();
  delay(1000);
  setupWifi();
}

void handleSaveMQTT() {
  // Проверяем метод запроса
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  
  // Получаем значения из формы
  mqtt_enabled = server.hasArg("mqtt_enabled");
  mqtt_server = server.arg("mqtt_server");
  mqtt_port = server.arg("mqtt_port");
  mqtt_user = server.arg("mqtt_user");
  mqtt_password = server.arg("mqtt_password");
  mqtt_prefix = server.arg("mqtt_prefix");
  
  Serial.println("Saving MQTT settings:");
  Serial.println("Enabled: " + String(mqtt_enabled ? "true" : "false"));
  Serial.println("Server: " + mqtt_server);
  Serial.println("Port: " + mqtt_port);
  Serial.println("User: " + mqtt_user);
  Serial.println("Prefix: " + mqtt_prefix);
  
  // Сохраняем в энергонезависимую память
  preferences.begin("mqtt", false);
  preferences.clear(); // Очищаем старые настройки
  preferences.putBool("enabled", mqtt_enabled);
  preferences.putString("server", mqtt_server);
  preferences.putString("port", mqtt_port);
  preferences.putString("user", mqtt_user);
  preferences.putString("password", mqtt_password);
  preferences.putString("prefix", mqtt_prefix);
  preferences.end();
  
  Serial.println("MQTT settings saved to NVS");
  
  // Отправляем ответ пользователю
  server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='3;url=/'></head><body><h1>MQTT settings saved!</h1><p>Redirecting in 3 seconds...</p></body></html>");
}

// Обработчик сброса WiFi настроек
void handleResetWifi() {
  Serial.println("Received WiFi reset request via web interface");
  server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='5;url=/'></head><body><h1>WiFi settings have been reset!</h1><p>The device will restart as an Access Point.</p><p>Redirecting in 5 seconds...</p></body></html>");
  delay(1000);
  resetWiFiSettings();
}

// Функция сброса WiFi настроек
void resetWiFiSettings() {
  Serial.println("Resetting WiFi settings...");
  
  // Включаем режим быстрого мигания светодиода
  isResetting = true;
  
  // Удаляем настройки WiFi из памяти
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
  
  // Сбрасываем переменные
  ssid = "";
  password = "";
  
  // Перезапускаем ESP в режиме точки доступа
  ap_mode = true;
  WiFi.disconnect();
  delay(1000);
  
  // Отключаем режим быстрого мигания
  isResetting = false;
  
  setupWifi();
}

// Настройка WiFi (точка доступа или клиент)
void setupWifi() {
  // Читаем сохраненные настройки
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  ap_mode = preferences.getBool("ap_mode", true); // Читаем сохраненный режим
  preferences.end();
  
  // Читаем настройки MQTT
  preferences.begin("mqtt", true);
  mqtt_enabled = preferences.getBool("enabled", false);
  mqtt_server = preferences.getString("server", "");
  mqtt_port = preferences.getString("port", "1883");
  mqtt_user = preferences.getString("user", "");
  mqtt_password = preferences.getString("password", "");
  mqtt_prefix = preferences.getString("prefix", "homeassistant");
  preferences.end();
  
  // Получаем MAC адрес и формируем имя устройства, если еще не сформировано
  if (deviceName == "") {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[7];
    sprintf(macStr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    deviceName = String(DEVICE_PREFIX) + String(macStr);
    mqtt_client_id = deviceName; // Устанавливаем ID клиента MQTT
  }
  
  // Устанавливаем имя хоста
  WiFi.setHostname(deviceName.c_str());
  
  // Режим работы в зависимости от наличия настроек
  if (ssid.length() > 0) {
    // Клиентский режим
    Serial.println("Connecting to WiFi: " + ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Пытаемся подключиться с ограничением по времени
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("");
      Serial.println("WiFi connected");
      Serial.println("IP address: " + WiFi.localIP().toString());
      ap_mode = false;
    } else {
      Serial.println("");
      Serial.println("Failed to connect to WiFi");
      startAPMode(); // Если не удалось подключиться, запускаем точку доступа
    }
  } else {
    // Режим точки доступа
    startAPMode();
  }
}

// Запуск режима точки доступа
void startAPMode() {
  Serial.println("Starting Access Point Mode");
  WiFi.mode(WIFI_AP);
  
  // Получаем MAC адрес и формируем имя устройства
  if (deviceName == "") {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[7];
    sprintf(macStr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    deviceName = String(DEVICE_PREFIX) + String(macStr);
    Serial.println("Device name: " + deviceName);
  }
  
  // Запускаем открытую точку доступа (без пароля)
  WiFi.softAP(deviceName.c_str());
  
  Serial.println("AP started");
  Serial.println("AP IP address: " + WiFi.softAPIP().toString());
  
  // Запускаем DNS сервер для перенаправления на веб-интерфейс
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  ap_mode = true;
  isConnected = false;  // Сбрасываем флаг подключения
}

// Настройка веб-сервера
void setupServer() {
  Serial.println("\nSetting up web server...");
  
  // Регистрируем обработчики для разных URL
  server.on("/", HTTP_GET, handleRoot);
  
  // Обработчик OTA обновления
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='5;url=/'></head><body><h1>Update complete!</h1><p>Device will restart in 5 seconds...</p></body></html>");
    delay(1000);
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
  
  server.on("/save-wifi", HTTP_POST, [](){
    if (server.hasArg("ssid") && server.hasArg("password")) {
      ssid = server.arg("ssid");
      password = server.arg("password");
      
      // Сохраняем настройки
      preferences.begin("wifi", false);
      preferences.clear();
      preferences.putString("ssid", ssid);
      preferences.putString("password", password);
      preferences.putBool("ap_mode", false);
      preferences.end();
      
      // Отправляем ответ
      server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='5;url=/'></head><body><h1>WiFi settings saved!</h1><p>The device will try to connect to the WiFi network.</p><p>If connection fails, the Access Point will be restarted.</p><p>Redirecting in 5 seconds...</p></body></html>");
      
      // Перезапускаем WiFi
      ap_mode = false;
      WiFi.disconnect();
      delay(1000);
      setupWifi();
    } else {
      server.send(400, "text/plain", "Missing parameters");
    }
  });
  
  server.on("/save-mqtt", HTTP_POST, [](){
    if (server.hasArg("mqtt_server") && server.hasArg("mqtt_port")) {
      mqtt_enabled = server.hasArg("mqtt_enabled");
      mqtt_server = server.arg("mqtt_server");
      mqtt_port = server.arg("mqtt_port");
      mqtt_user = server.arg("mqtt_user");
      mqtt_password = server.arg("mqtt_password");
      mqtt_prefix = server.arg("mqtt_prefix");
      
      // Сохраняем настройки
      preferences.begin("mqtt", false);
      preferences.clear();
      preferences.putBool("enabled", mqtt_enabled);
      preferences.putString("server", mqtt_server);
      preferences.putString("port", mqtt_port);
      preferences.putString("user", mqtt_user);
      preferences.putString("password", mqtt_password);
      preferences.putString("prefix", mqtt_prefix);
      preferences.end();
      
      server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='3;url=/'></head><body><h1>MQTT settings saved!</h1><p>Redirecting in 3 seconds...</p></body></html>");
    } else {
      server.send(400, "text/plain", "Missing parameters");
    }
  });
  
  server.on("/reset-wifi", HTTP_POST, [](){
    server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='5;url=/'></head><body><h1>WiFi settings have been reset!</h1><p>The device will restart as an Access Point.</p><p>Redirecting in 5 seconds...</p></body></html>");
    delay(1000);
    resetWiFiSettings();
  });
  
  server.on("/mqtt-status", HTTP_GET, handleMqttStatus);
  
  server.begin();
  Serial.println("Web server started");
}

// Настройка OTA
void setupOTA() {
  Serial.println("Setting up OTA...");
  
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else
        type = "filesystem";
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

// Проверка нажатия кнопки BOOT
void checkResetButton() {
  static unsigned long pressStartTime = 0;
  static bool buttonPressed = false;
  
  if (digitalRead(BOOT_BUTTON) == LOW) {  // Кнопка нажата (активный LOW)
    if (!buttonPressed) {
      buttonPressed = true;
      pressStartTime = millis();
    }
    
    // Если кнопка удерживается более 3 секунд, сбрасываем настройки WiFi
    if (buttonPressed && (millis() - pressStartTime > 3000)) {
      Serial.println("BOOT button held for 3 seconds - resetting WiFi settings");
      resetWiFiSettings();
      pressStartTime = millis(); // Сбрасываем таймер, чтобы избежать повторного срабатывания
    }
  } else {
    buttonPressed = false; // Кнопка отпущена
  }
}

// Проверка подключения клиентов к AP или подключения к WiFi
void checkConnections() {
  bool prevConnected = isConnected;
  
  if (ap_mode) {
    // В режиме AP проверяем подключение клиентов
    isConnected = WiFi.softAPgetStationNum() > 0;
  } else {
    // В режиме клиента проверяем подключение к WiFi
    isConnected = WiFi.status() == WL_CONNECTED;
  }
  
  // Если состояние подключения изменилось, выводим информацию
  if (prevConnected != isConnected) {
    if (isConnected) {
      Serial.println("Connection established!");
    } else {
      Serial.println("Connection lost!");
    }
  }
}

// Обновление состояния светодиода
void updateLedState() {
  unsigned long currentMillis = millis();
  
  if (isResetting) {
    // Быстрое мигание во время сброса (каждые 100 мс)
    if (currentMillis - ledLastToggle > 100) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      ledLastToggle = currentMillis;
    }
  } else if (isConnected) {
    // Постоянно горит при наличии подключения
    if (!ledState) {
      ledState = true;
      digitalWrite(LED_PIN, HIGH);
    }
  } else {
    // Медленное мигание при отсутствии подключения (каждые 500 мс)
    if (currentMillis - ledLastToggle > 500) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      ledLastToggle = currentMillis;
    }
  }
}

// Функция загрузки настроек
void loadSettings() {
  Serial.println("Loading settings from NVS...");
  
  // Загружаем WiFi настройки
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  ap_mode = preferences.getBool("ap_mode", true);
  preferences.end();
  
  // Загружаем MQTT настройки
  preferences.begin("mqtt", true);
  mqtt_enabled = preferences.getBool("enabled", false);
  mqtt_server = preferences.getString("server", "");
  mqtt_port = preferences.getString("port", "1883");
  mqtt_user = preferences.getString("user", "");
  mqtt_password = preferences.getString("password", "");
  mqtt_prefix = preferences.getString("prefix", "homeassistant");
  preferences.end();
  
  Serial.println("Settings loaded successfully");
  Serial.println("MQTT Prefix: " + mqtt_prefix);
}

// Функция для получения тестовых значений сенсоров
void updateSensorValues() {
  // Заглушка для тестовых значений
  static unsigned long lastUpdate = 0;
  const unsigned long UPDATE_INTERVAL = 5000; // 5 секунд
  
  if (millis() - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = millis();
    
    // Генерируем тестовые значения
    orpValue = 650.0 + random(-10, 10); // ORP в диапазоне 640-660 mV
    phValue = 7.2 + random(-2, 2) * 0.1; // pH в диапазоне 7.0-7.4
    
    safePrintln("Sensor values updated:");
    safePrintln("ORP: " + String(orpValue) + " mV");
    safePrintln("pH: " + String(phValue));
  }
}

// Улучшенная функция setup
void setup() {
  // Инициализация последовательного порта
  Serial.begin(115200);
  delay(1000);
  
  // Ждем инициализации порта
  while (!Serial) {
    delay(10);
  }
  
  Serial.println("\n\n=== ESP32 ORP/pH Device ===");
  Serial.println("Version: " + String(VERSION));
  Serial.println("===========================\n");
  
  // Инициализация GPIO
  pinMode(BOOT_BUTTON, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Загрузка настроек
  loadSettings();
  
  // Настройка WiFi
  setupWifi();
  
  // Настройка OTA (только в режиме клиента)
  if (!ap_mode) {
    setupOTA();
  }
  
  // Настройка веб-сервера
  setupServer();
  
  // Устанавливаем ID клиента MQTT как имя устройства
  mqtt_client_id = deviceName;
  
  // Настройка MQTT клиента
  mqttClient.setServer(mqtt_server.c_str(), atoi(mqtt_port.c_str()));
  mqttClient.setBufferSize(MQTT_MAX_PACKET_SIZE);  // Устанавливаем размер буфера
  
  // Пробуем подключиться к MQTT
  if (mqtt_enabled) {
    connectToMQTT();
  }
  
  Serial.println("Setup completed\n");
}

// Функция для проверки подключения к WiFi
bool checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    debugPrint("WiFi connection lost");
    return false;
  }
  return true;
}

// Добавляем обработчик для получения статуса MQTT
void handleMqttStatus() {
  String status = getMqttStatus();
  String json = "{\"status\":\"" + status + "\"}";
  server.send(200, "application/json", json);
}

// Функция для отправки данных в MQTT
void sendMqttData() {
  if (!mqttClient.connected()) {
    safePrintln("MQTT: Not connected, skipping data send");
    return;
  }

  // Базовый топик для устройства
  String baseTopic = mqtt_prefix + "/sensor/" + mqtt_client_id;
  
  // Топик для состояния
  String stateTopic = baseTopic + "/state";

  // Конфигурация для ORP сенсора
  String orpConfigTopic = baseTopic + "/orp/config";
  
  // Конфигурация для pH сенсора
  String phConfigTopic = baseTopic + "/ph/config";

  // Получаем MAC адрес для идентификатора устройства
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Конфигурация ORP сенсора (упрощенная)
  String orpConfig = "{\"name\":\"" + deviceName + " ORP\","
                     "\"uniq_id\":\"" + mqtt_client_id + "_orp\","
                     "\"dev_cla\":\"voltage\","
                     "\"stat_cla\":\"measurement\","
                     "\"unit_of_meas\":\"mV\","
                     "\"dev\":{\"ids\":[\"" + String(macStr) + "\"],"
                             "\"name\":\"" + deviceName + "\","
                             "\"mf\":\"eyera\","
                             "\"mdl\":\"orp_ph\"},"
                     "\"stat_t\":\"" + stateTopic + "\","
                     "\"val_tpl\":\"{{ value_json.orp }}\","
                     "\"frc_upd\":true,"
                     "\"exp_aft\":300}";

  // Конфигурация pH сенсора (упрощенная)
  String phConfig = "{\"name\":\"" + deviceName + " pH\","
                    "\"uniq_id\":\"" + mqtt_client_id + "_ph\","
                    "\"dev_cla\":\"ph\","
                    "\"stat_cla\":\"measurement\","
                    "\"unit_of_meas\":\"pH\","
                    "\"dev\":{\"ids\":[\"" + String(macStr) + "\"],"
                            "\"name\":\"" + deviceName + "\","
                            "\"mf\":\"eyera\","
                            "\"mdl\":\"orp_ph\"},"
                    "\"stat_t\":\"" + stateTopic + "\","
                    "\"val_tpl\":\"{{ value_json.ph }}\","
                    "\"frc_upd\":true,"
                    "\"exp_aft\":300}";

  // Выводим отладочную информацию
  safePrintln("\n=== MQTT Configuration ===");
  safePrintln("Base Topic: " + baseTopic);
  safePrintln("State Topic: " + stateTopic);
  safePrintln("ORP Config Topic: " + orpConfigTopic);
  safePrintln("pH Config Topic: " + phConfigTopic);
  safePrintln("Device MAC: " + String(macStr));
  safePrintln("ORP Config: " + orpConfig);
  safePrintln("pH Config: " + phConfig);
  safePrintln("========================\n");

  // Проверяем размер сообщений
  safePrintln("ORP Config size: " + String(orpConfig.length()));
  safePrintln("pH Config size: " + String(phConfig.length()));

  // Отправляем конфигурацию с retain=true
  if (orpConfig.length() > 0) {
    bool orpConfigSent = mqttClient.publish(orpConfigTopic.c_str(), (uint8_t*)orpConfig.c_str(), orpConfig.length(), true);
    if (orpConfigSent) {
      safePrintln("ORP config published successfully");
    } else {
      safePrintln("Failed to publish ORP config. Error: " + String(mqttClient.state()));
      safePrintln("MQTT Client state: " + String(mqttClient.state()));
    }
  } else {
    safePrintln("ORP config is empty, skipping publish");
  }

  // Даем время на отправку первого сообщения
  delay(200);

  if (phConfig.length() > 0) {
    bool phConfigSent = mqttClient.publish(phConfigTopic.c_str(), (uint8_t*)phConfig.c_str(), phConfig.length(), true);
    if (phConfigSent) {
      safePrintln("pH config published successfully");
    } else {
      safePrintln("Failed to publish pH config. Error: " + String(mqttClient.state()));
      safePrintln("MQTT Client state: " + String(mqttClient.state()));
    }
  } else {
    safePrintln("pH config is empty, skipping publish");
  }

  // Даем время на отправку второго сообщения
  delay(200);

  // Отправляем текущие значения с retain=false
  String stateData = "{\"orp\":" + String(orpValue) + ",\"ph\":" + String(phValue) + "}";
  if (stateData.length() > 0) {
    bool stateSent = mqttClient.publish(stateTopic.c_str(), (uint8_t*)stateData.c_str(), stateData.length(), false);
    if (stateSent) {
      safePrintln("State data published successfully");
    } else {
      safePrintln("Failed to publish state data. Error: " + String(mqttClient.state()));
      safePrintln("MQTT Client state: " + String(mqttClient.state()));
    }
  } else {
    safePrintln("State data is empty, skipping publish");
  }
}

// Модифицируем функцию loop
void loop() {
  static unsigned long lastCheck = 0;
  static unsigned long lastMqttSend = 0;
  const unsigned long CHECK_INTERVAL = 100; // 100ms
  const unsigned long MQTT_SEND_INTERVAL = 60000; // 60 секунд
  
  unsigned long now = millis();
  
  // Периодические проверки
  if (now - lastCheck >= CHECK_INTERVAL) {
    lastCheck = now;
    
    // Обновляем значения сенсоров
    updateSensorValues();
    
    // Проверка подключений
    checkConnections();
    
    // Обновление LED
    updateLedState();
    
    // Проверка кнопки сброса
    checkResetButton();
    
    // Обработка MQTT
    if (mqtt_enabled) {
      mqttClient.loop();
      
      // Отправка данных в MQTT
      if (now - lastMqttSend >= MQTT_SEND_INTERVAL) {
        lastMqttSend = now;
        sendMqttData();
      }
    }
  }
  
  // Обработка DNS и HTTP запросов
  if (ap_mode) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  
  // Обработка OTA (только в режиме клиента)
  if (!ap_mode) {
    ArduinoOTA.handle();
  }
  
  // Даем время другим задачам
  yield();
} 