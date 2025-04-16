#include <WiFi.h>        // Библиотека для работы с WiFi
#include <WebServer.h>    // Библиотека для создания веб-сервера
#include <DNSServer.h>    // Библиотека для DNS-сервера (перенаправление на страницу настройки)
#include <Preferences.h>  // Библиотека для хранения настроек в энергонезависимой памяти

// Константы и параметры
#define BOOT_BUTTON 0   // GPIO0 для кнопки BOOT
#define LED_PIN     2   // Светодиод индикации (обычно GPIO2 на большинстве ESP32)
const char* DEVICE_PREFIX = "ORP_pH_"; // Префикс имени устройства
String deviceName = ""; // Полное имя устройства с MAC
const byte DNS_PORT = 53;

// Объекты для работы
WebServer server(80);   // Веб-сервер на порту 80
DNSServer dnsServer;    // DNS-сервер для перенаправления на страницу настройки
Preferences preferences; // Объект для работы с энергонезависимой памятью

// Переменные для хранения настроек
String ssid = "";         // Имя WiFi сети
String password = "";     // Пароль WiFi сети
String mqtt_server = "";  // Адрес MQTT сервера
String mqtt_port = "1883"; // Порт MQTT сервера
String mqtt_user = "";    // Имя пользователя MQTT
String mqtt_password = ""; // Пароль MQTT
String mqtt_topic = "sensors/orp_ph"; // Топик MQTT
bool mqtt_enabled = false; // Включен ли MQTT

// Режим работы: true - точка доступа, false - клиент
bool ap_mode = true;

// Переменные состояния
bool ledState = false;          // Текущее состояние светодиода
bool isConnected = false;       // Флаг подключения к сети или клиента к AP
bool isResetting = false;       // Флаг процесса сброса
unsigned long ledLastToggle = 0; // Время последнего переключения светодиода

// Обработка страницы конфигурации
void handleRoot() {
  server.send(200, "text/html", getAPConfigPage());
}

// Страница конфигурации точки доступа и WiFi
String getAPConfigPage() {
  // Формируем HTML-страницу с настройками
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Eyera ORP/pH sensor</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:20px;background-color:#f5f5f5;}";
  html += "h1{color:#0066cc;}";
  html += ".container{background-color:white;border-radius:10px;padding:20px;box-shadow:0 4px 8px rgba(0,0,0,0.1);}";
  html += "label{display:block;margin-top:10px;font-weight:bold;}";
  html += "input[type=text],input[type=password],input[type=number]{width:100%;padding:8px;margin:6px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}";
  html += "input[type=checkbox]{margin:10px 5px 10px 0;}";
  html += "input[type=submit]{background-color:#0066cc;color:white;padding:10px 15px;border:none;border-radius:4px;cursor:pointer;margin-top:15px;}";
  html += "input[type=submit]:hover{background-color:#0055bb;}";
  html += ".section{margin-bottom:20px;border-bottom:1px solid #eee;padding-bottom:20px;}";
  html += "input[type=submit].warning{background-color:#cc3300;}";
  html += "input[type=submit].warning:hover{background-color:#bb2200;}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Eyera ORP/pH sensor</h1>";
  
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
  
  // Секция настроек MQTT
  html += "<div class='section'>";
  html += "<h2>MQTT Settings</h2>";
  html += "<form action='/save-mqtt' method='POST'>";
  html += "<label><input type='checkbox' name='mqtt_enabled' " + String(mqtt_enabled ? "checked" : "") + "> Enable MQTT</label>";
  html += "<label for='mqtt_server'>MQTT Server:</label>";
  html += "<input type='text' name='mqtt_server' value='" + mqtt_server + "'>";
  html += "<label for='mqtt_port'>MQTT Port:</label>";
  html += "<input type='number' name='mqtt_port' value='" + mqtt_port + "'>";
  html += "<label for='mqtt_user'>MQTT Username:</label>";
  html += "<input type='text' name='mqtt_user' value='" + mqtt_user + "'>";
  html += "<label for='mqtt_password'>MQTT Password:</label>";
  html += "<input type='password' name='mqtt_password' value='" + mqtt_password + "'>";
  html += "<label for='mqtt_topic'>MQTT Topic:</label>";
  html += "<input type='text' name='mqtt_topic' value='" + mqtt_topic + "'>";
  html += "<input type='submit' value='Save MQTT Settings'>";
  html += "</form>";
  html += "</div>";
  
  // Добавляем кнопку сброса настроек только в режиме клиента (станции)
  if (!ap_mode) {
    html += "<div class='section'>";
    html += "<h2>Reset WiFi Settings</h2>";
    html += "<form action='/reset-wifi' method='POST'>";
    html += "<input type='submit' class='warning' value='Reset WiFi Settings'>";
    html += "<p>This will erase all WiFi settings and restart the device in Access Point mode.</p>";
    html += "</form>";
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
  
  // Сохраняем в энергонезависимую память
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  
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
  mqtt_topic = server.arg("mqtt_topic");
  
  // Сохраняем в энергонезависимую память
  preferences.begin("mqtt", false);
  preferences.putBool("enabled", mqtt_enabled);
  preferences.putString("server", mqtt_server);
  preferences.putString("port", mqtt_port);
  preferences.putString("user", mqtt_user);
  preferences.putString("password", mqtt_password);
  preferences.putString("topic", mqtt_topic);
  preferences.end();
  
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
  preferences.end();
  
  // Читаем настройки MQTT
  preferences.begin("mqtt", true);
  mqtt_enabled = preferences.getBool("enabled", false);
  mqtt_server = preferences.getString("server", "");
  mqtt_port = preferences.getString("port", "1883");
  mqtt_user = preferences.getString("user", "");
  mqtt_password = preferences.getString("password", "");
  mqtt_topic = preferences.getString("topic", "sensors/orp_ph");
  preferences.end();
  
  // Получаем MAC адрес и формируем имя устройства, если еще не сформировано
  if (deviceName == "") {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[7];
    sprintf(macStr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    deviceName = String(DEVICE_PREFIX) + String(macStr);
    Serial.println("Device name: " + deviceName);
  }
  
  // Устанавливаем имя хоста
  WiFi.setHostname(deviceName.c_str());
  
  // Режим работы в зависимости от наличия настроек
  if (ssid.length() > 0 && !ap_mode) {
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
  // Регистрируем обработчики для разных URL
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save-wifi", HTTP_POST, handleSaveWifi);
  server.on("/save-mqtt", HTTP_POST, handleSaveMQTT);
  server.on("/reset-wifi", HTTP_POST, handleResetWifi);
  
  // Обработчик для всех остальных URL, чтобы избежать ошибок 404
  server.onNotFound([]() {
    server.send(200, "text/html", getAPConfigPage());
  });
  
  server.begin();
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

void setup() {
  // Инициализация последовательного порта для отладки
  Serial.begin(115200);
  Serial.println("Starting ESP32 ORP/pH Device");
  
  // Настройка кнопки BOOT и светодиода
  pinMode(BOOT_BUTTON, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Настройка WiFi
  setupWifi();
  
  // Настройка веб-сервера
  setupServer();
  
  Serial.println("Setup completed");
}

void loop() {
  // Обработка DNS запросов в режиме точки доступа
  if (ap_mode) {
    dnsServer.processNextRequest();
  }
  
  // Обработка HTTP запросов
  server.handleClient();
  
  // Проверка нажатия кнопки BOOT для сброса WiFi
  checkResetButton();
  
  // Проверка подключений
  checkConnections();
  
  // Обновление состояния светодиода
  updateLedState();
  
  // Задержка для стабильной работы
  delay(10);
} 