#include <WiFi.h>        // Библиотека для работы с WiFi
#include <WebServer.h>    // Библиотека для создания веб-сервера
#include <DNSServer.h>    // Библиотека для DNS-сервера (перенаправление на страницу настройки)
#include <Preferences.h>  // Библиотека для хранения настроек в энергонезависимой памяти
#include <Update.h>       // Библиотека для OTA обновления
#include <HTTPUpdate.h>   // Библиотека для HTTP OTA обновления

// Константы и параметры
#define BOOT_BUTTON 0   // GPIO0 для кнопки BOOT
#define LED_PIN     2   // Светодиод индикации (GPIO2)
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
String mqtt_client_id = ""; // ID клиента MQTT (будет установлен как имя устройства)

// Режим работы: true - точка доступа, false - клиент
bool ap_mode = true;

// Переменные состояния
bool ledState = false;          // Текущее состояние светодиода
bool isConnected = false;       // Флаг подключения к сети или клиента к AP
bool isResetting = false;       // Флаг процесса сброса
bool isUpdating = false;        // Флаг процесса OTA обновления
unsigned long ledLastToggle = 0; // Время последнего переключения светодиода

// Обработка страницы конфигурации
void handleRoot() {
  server.send(200, "text/html", getAPConfigPage());
}

// Обработчик для OTA обновления
void handleOTAUpdate() {
  Serial.println("=== OTA Update Handler ===");
  Serial.println("Current version: v1.0.1");
  Serial.println("Starting OTA update process...");
  
  HTTPUpload& upload = server.upload();
  static bool updateStarted = false;
  static size_t totalSize = 0;
  
  Serial.println("OTA Update handler called");
  Serial.printf("Upload status: %d\n", upload.status);
  Serial.printf("Upload filename: %s\n", upload.filename.c_str());
  Serial.printf("Upload name: %s\n", upload.name.c_str());
  Serial.printf("Upload type: %s\n", upload.type.c_str());
  Serial.printf("Upload totalSize: %u\n", upload.totalSize);
  Serial.printf("Upload currentSize: %u\n", upload.currentSize);
  
  if (upload.status == UPLOAD_FILE_START) {
    if (isUpdating) {
      Serial.println("Update already in progress");
      server.send(500, "text/plain", "Update already in progress");
      return;
    }
    
    // Выводим текущие настройки
    Serial.println("Current settings before update:");
    Serial.println("WiFi SSID: " + ssid);
    Serial.println("WiFi Password: " + password);
    Serial.println("MQTT Server: " + mqtt_server);
    Serial.println("MQTT Port: " + mqtt_port);
    Serial.println("MQTT User: " + mqtt_user);
    Serial.println("MQTT Topic: " + mqtt_topic);
    Serial.println("MQTT Enabled: " + String(mqtt_enabled ? "true" : "false"));
    
    Serial.printf("Update: %s\n", upload.filename.c_str());
    Serial.printf("Starting update...\n");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Free sketch space: %u bytes\n", ESP.getFreeSketchSpace());
    
    // Проверяем, что это бинарный файл
    if (upload.filename.indexOf(".bin") == -1) {
      Serial.println("Error: Not a binary file");
      server.send(500, "text/plain", "Error: Not a binary file");
      return;
    }
    
    isUpdating = true;
    updateStarted = false;
    totalSize = 0;
    Serial.println("Waiting for file upload...");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!isUpdating) {
      Serial.println("Update not started");
      server.send(500, "text/plain", "Update not started");
      return;
    }
    
    // Накапливаем общий размер
    totalSize += upload.currentSize;
    Serial.printf("Received chunk: %u bytes, total: %u bytes\n", upload.currentSize, totalSize);
    
    // Если обновление еще не начато и мы получили достаточно данных
    if (!updateStarted && totalSize > 1024) {
      Serial.printf("Starting update, received size: %u bytes\n", totalSize);
      Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
      Serial.printf("Free sketch space: %u bytes\n", ESP.getFreeSketchSpace());
      
      // Проверяем, что файл не слишком большой
      if (totalSize > (ESP.getFreeSketchSpace() - 0x1000)) {
        Serial.printf("Error: File too large (%u > %u)\n", totalSize, ESP.getFreeSketchSpace() - 0x1000);
        server.send(500, "text/plain", "Error: File too large");
        isUpdating = false;
        return;
      }
      
      // Начинаем обновление с сохранением настроек
      if (!Update.begin(totalSize, U_FLASH)) {
        Serial.printf("Update begin failed: ");
        Update.printError(Serial);
        server.send(500, "text/plain", "Update begin failed");
        isUpdating = false;
        return;
      }
      updateStarted = true;
      Serial.printf("Update begin successful, size: %u bytes\n", totalSize);
    }
    
    // Если обновление начато, записываем данные
    if (updateStarted) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Serial.printf("Update write failed: ");
        Update.printError(Serial);
        server.send(500, "text/plain", "Update write failed");
        isUpdating = false;
        updateStarted = false;
        return;
      }
      
      // Выводим прогресс
      static uint8_t lastProgress = 0;
      uint8_t currentProgress = (totalSize * 100) / upload.totalSize;
      if (currentProgress != lastProgress) {
        lastProgress = currentProgress;
        Serial.printf("Progress: %u%% (%u/%u bytes)\n", currentProgress, totalSize, upload.totalSize);
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!isUpdating || !updateStarted) {
      Serial.println("Update not started");
      server.send(500, "text/plain", "Update not started");
      return;
    }
    
    Serial.printf("Update file end, total size: %u bytes\n", totalSize);
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    
    // Завершаем обновление
    if (!Update.end(true)) {
      Serial.printf("Update failed: ");
      Update.printError(Serial);
      server.send(500, "text/plain", "Update failed");
      isUpdating = false;
      updateStarted = false;
      return;
    }
    
    Serial.printf("Update Success: %u bytes\n", totalSize);
    Serial.println("Rebooting...");
    
    // Отправляем страницу успешного обновления
    String successPage = "<!DOCTYPE html><html><head>";
    successPage += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    successPage += "<title>Update Success</title>";
    successPage += "<style>";
    successPage += "body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;margin:0;padding:0;background-color:#121212;color:#e0e0e0;}";
    successPage += ".container{max-width:800px;margin:20px auto;padding:20px;background-color:#1e1e1e;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.3);}";
    successPage += "h1{color:#4CAF50;margin-bottom:20px;font-size:24px;font-weight:500;}";
    successPage += "p{color:#b0b0b0;font-size:14px;line-height:1.5;}";
    successPage += "</style>";
    successPage += "<meta http-equiv='refresh' content='5;url=/'></head><body>";
    successPage += "<div class='container'>";
    successPage += "<h1>Update Success!</h1>";
    successPage += "<p>Firmware has been successfully updated.</p>";
    successPage += "<p>Device will reboot in 5 seconds...</p>";
    successPage += "</div></body></html>";
    
    server.send(200, "text/html", successPage);
    
    // Даем время на отправку ответа
    delay(1000);
    
    // Перезагружаем устройство
    ESP.restart();
  }
}

// Страница конфигурации точки доступа и WiFi
String getAPConfigPage() {
  // Формируем HTML-страницу с настройками
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
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
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<div class='header'>";
  html += "<h1>Eyera " + deviceName + " sensor</h1>";
  html += "<span class='version'>v1.0.1</span>";
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

    // Добавляем секцию OTA обновления только в режиме клиента
    html += "<div class='section'>";
    html += "<h2>OTA Update</h2>";
    html += "<form action='/update' method='POST' enctype='multipart/form-data'>";
    html += "<label for='update'>Select firmware file:</label>";
    html += "<input type='file' name='update' id='update' accept='.bin' required>";
    html += "<input type='submit' value='Upload and Update'>";
    html += "<p class='info-text'>Upload a new firmware file to update the device.</p>";
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
  mqtt_topic = server.arg("mqtt_topic");
  
  Serial.println("Saving MQTT settings:");
  Serial.println("Enabled: " + String(mqtt_enabled ? "true" : "false"));
  Serial.println("Server: " + mqtt_server);
  Serial.println("Port: " + mqtt_port);
  Serial.println("User: " + mqtt_user);
  Serial.println("Topic: " + mqtt_topic);
  
  // Сохраняем в энергонезависимую память
  preferences.begin("mqtt", false);
  preferences.clear(); // Очищаем старые настройки
  preferences.putBool("enabled", mqtt_enabled);
  preferences.putString("server", mqtt_server);
  preferences.putString("port", mqtt_port);
  preferences.putString("user", mqtt_user);
  preferences.putString("password", mqtt_password);
  preferences.putString("topic", mqtt_topic);
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
  mqtt_topic = preferences.getString("topic", "sensors/orp_ph");
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
  // Регистрируем обработчики для разных URL
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save-wifi", HTTP_POST, handleSaveWifi);
  server.on("/save-mqtt", HTTP_POST, handleSaveMQTT);
  server.on("/reset-wifi", HTTP_POST, handleResetWifi);
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", "Update Success!");
  }, handleOTAUpdate);
  
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
  
  // Даем время на инициализацию NVS
  delay(100);
  
  // Читаем и выводим настройки WiFi
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  preferences.end();
  
  Serial.println("Current WiFi settings:");
  Serial.println("SSID: " + ssid);
  Serial.println(String("Password: ") + (password.length() > 0 ? "********" : "not set"));
  
  // Читаем и выводим настройки MQTT
  preferences.begin("mqtt", true);
  mqtt_enabled = preferences.getBool("enabled", false);
  mqtt_server = preferences.getString("server", "");
  mqtt_port = preferences.getString("port", "1883");
  mqtt_user = preferences.getString("user", "");
  mqtt_password = preferences.getString("password", "");
  mqtt_topic = preferences.getString("topic", "sensors/orp_ph");
  preferences.end();
  
  Serial.println("Current MQTT settings:");
  Serial.println("Enabled: " + String(mqtt_enabled ? "true" : "false"));
  Serial.println("Server: " + mqtt_server);
  Serial.println("Port: " + mqtt_port);
  Serial.println("User: " + mqtt_user);
  Serial.println("Topic: " + mqtt_topic);
  
  // Выводим информацию о памяти
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Free sketch space: %u bytes\n", ESP.getFreeSketchSpace());
  Serial.printf("Sketch size: %u bytes\n", ESP.getSketchSize());
  Serial.printf("Flash size: %u bytes\n", ESP.getFlashChipSize());
  
  // Настройка кнопки BOOT и светодиода
  pinMode(BOOT_BUTTON, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Настройка WiFi
  setupWifi();
  
  // Настройка веб-сервера
  setupServer();
  
  // Инициализация OTA
  Update.onProgress([](size_t progress, size_t total) {
    static uint8_t lastProgress = 0;
    uint8_t currentProgress = (progress * 100) / total;
    if (currentProgress != lastProgress) {
      lastProgress = currentProgress;
      Serial.printf("OTA Progress: %u%%\n", currentProgress);
    }
  });
  
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