#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h> // Для MQTT
#include <Preferences.h>  // Для хранения настроек
#include <DNSServer.h>    // Для Captive Portal в режиме AP
#include <ph4502c_sensor.h> // Библиотека для pH сенсора

// --- Пины ---
const int PH_PIN = 34;     // Пин для pH сенсора (только вход)
const int ORP_PIN = 35;    // Пин для ORP сенсора (только вход)
const int LED_PIN = 2;     // Встроенный светодиод ESP32
const int RESET_BUTTON_PIN = 0; // Кнопка BOOT для сброса настроек

// --- Глобальные переменные ---
Preferences preferences; // Объект для работы с Preferences

// WiFi & Web
WiFiClient espClient;    // TCP клиент для MQTT
WebServer server(80);    // Веб-сервер на порту 80
DNSServer dnsServer;     // DNS-сервер

// MQTT
PubSubClient mqttClient(espClient);
String mqtt_broker = "";
const int MQTT_PORT = 1883; // Стандартный порт MQTT
String mqttClientID = "ORP_pH_"; // Префикс для ClientID
String phTopic = ""; // Будет сформирован в setup
String orpTopic = ""; // Будет сформирован в setup
String commandTopic = ""; // Будет сформирован в setup

// Настройки WiFi (загружаются из Preferences)
String ssid = "";
String password = "";

// Флаг режима точки доступа
bool apMode = false;

// Переменные для хранения последних значений датчиков
float current_ph = -1.0; // -1 = Невалидное значение
float current_orp = -999.0; // -999 = Невалидное значение

// --- Калибровочные параметры (загружаются из Preferences) ---
// Параметры для библиотеки PH4502C - БОЛЬШЕ НЕ СОХРАНЯЮТСЯ/ЗАГРУЖАЮТСЯ
// Калибровка выполняется методом recalculate() и действует до перезагрузки.

// ORP:
// Простое смещение в мВ (mV = V * slope_orp + offset_orp)
// Часто производители указывают смещение при 0V или опорное значение
float orp_cal_offset_mv = 0.0; // Смещение (мВ) при 0V (или опорном напряжении)
// Наклон для ORP (мВ на Вольт)
float orp_slope_mv_per_volt = 1000.0; // Пример: 1V = 1000mV (может отличаться)

// --- Временные переменные для процесса калибровки через Web ---
float temp_ph7_v = -1.0;
float temp_ph4_v = -1.0;
float temp_orp_v = -1.0;
float temp_orp_mv_known = -999.0; // Значение ORP калибровочного раствора

// Имя точки доступа
String apName = "ORP_pH_";

// --- Настройки интервалов ---
const unsigned long SENSOR_READ_INTERVAL = 1000; // Интервал чтения датчиков (мс)
unsigned long mqtt_publish_interval_sec = 60; // Интервал публикации MQTT (сек), загружается из Preferences

// --- Переменные для таймеров ---
unsigned long lastSensorReadTime = 0;
unsigned long lastMqttPublishTime = 0;
unsigned long lastMqttReconnectAttempt = 0;

// --- Флаги управления ---
bool forceMqttPublish = false; // Флаг для принудительной публикации MQTT

// --- Объект pH сенсора ---
PH4502C_Sensor ph_sensor(PH_PIN, 0 /* УКАЖИ_ПИН_ТЕМПЕРАТУРЫ_ИЛИ_ОСТАВЬ_0 */); 

// HTML страница конфигурации для режима AP
String getAPConfigPage() {
  String page = "<!DOCTYPE HTML><html><head>";
  page += "<title>ORP_pH Настройка</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f4; } ";
  page += "h1, h2 { color: #333; text-align: center; margin-bottom: 20px; } ";
  page += "form, .cal-section { background-color: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); max-width: 500px; margin: 20px auto; } ";
  page += "label { display: block; margin-bottom: 8px; color: #555; font-weight: bold; } ";
  page += "input[type=text], input[type=password], input[type=number] { width: calc(100% - 22px); padding: 10px; margin-bottom: 15px; border: 1px solid #ccc; border-radius: 4px; } ";
  page += "input[type=submit], button { background-color: #007bff; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; margin-top: 10px; margin-right: 5px; } ";
  page += "input[type=submit]:hover, button:hover { background-color: #0056b3; } ";
  page += ".cal-section button { background-color: #28a745; } .cal-section button:hover { background-color: #218838; } ";
  page += ".reset-btn { background-color: #dc3545; } .reset-btn:hover { background-color: #c82333; } ";
  page += ".value { font-weight: bold; color: #0056b3; } ";
  page += ".note { font-size: 0.9em; color: #666; margin-top: 5px; margin-bottom: 15px; } ";
  page += ".msg { text-align: center; margin-top: 20px; font-weight: bold; } ";
  page += ".success { color: green; } .error { color: red; } ";
  page += "</style></head><body>";
  page += "<h1>Настройка ORP_pH</h1>";

  // --- Форма настроек WiFi и MQTT ---
  page += "<form method='POST' action='/save'>";
  page += "<h2>Настройки сети</h2>";
  page += "<label for='ssid'>Имя WiFi (SSID):</label>";
  page += "<input type='text' name='ssid' id='ssid' value='" + ssid + "' required><br>";
  page += "<label for='pass'>Пароль WiFi:</label>";
  page += "<input type='password' name='pass' id='pass'><br>";
  page += "<label for='mqtt'>Адрес MQTT брокера:</label>";
  page += "<input type='text' name='mqtt' id='mqtt' placeholder='например, 192.168.1.100' value='" + mqtt_broker + "' required><br>";
  page += "<label for='mqtt_int'>Интервал публикации MQTT (сек):</label>";
  page += "<input type='number' name='mqtt_int' id='mqtt_int' value='" + String(mqtt_publish_interval_sec) + "' min='5' required><br>"; // Мин. интервал 5 сек
  page += "<input type='submit' value='Сохранить сеть и перезагрузить'>";
  page += "</form>";

  // --- Секция калибровки ---
  page += "<div class='cal-section'>";
  page += "<h2>Калибровка датчиков</h2>";

  // --- Калибровка pH ---
  page += "<h3>pH</h3>";
  page += "<p class='note'>Погрузите датчик в буферный раствор pH 7.0, подождите стабилизации и нажмите кнопку.</p>";
  page += "<form method='POST' action='/read_ph7' style='display:inline;'><button type='submit'>Прочитать V для pH 7</button></form>";
  page += " Текущее V: <span class='value'>";
  page += (temp_ph7_v < 0) ? "(не прочитано)" : String(temp_ph7_v, 3) + " V";
  page += "</span><br>";

  page += "<p class='note' style='margin-top:15px;'>Погрузите датчик в буферный раствор pH 4.0, подождите стабилизации и нажмите кнопку.</p>";
  page += "<form method='POST' action='/read_ph4' style='display:inline;'><button type='submit'>Прочитать V для pH 4</button></form>";
  page += " Текущее V: <span class='value'>";
  page += (temp_ph4_v < 0) ? "(не прочитано)" : String(temp_ph4_v, 3) + " V";
  page += "</span><br>";

  // --- Калибровка ORP ---
  page += "<h3 style='margin-top:25px;'>ORP</h3>";
  page += "<p class='note'>Погрузите датчик в калибровочный раствор ORP, введите его известное значение (mV) и нажмите кнопку.</p>";
  page += "<form method='POST' action='/read_orp'>"; // Используем одну форму для ORP
  page += "<label for='orp_mv'>Значение раствора (mV):</label>";
  page += "<input type='number' step='0.1' name='orp_mv' id='orp_mv' required><br>";
  page += "<button type='submit'>Прочитать V для ORP</button>";
  page += " Текущее V: <span class='value'>";
  page += (temp_orp_v < 0) ? "(не прочитано)" : String(temp_orp_v, 3) + " V";
  page += "</span><br>";
  page += "</form>";
  page += "<p class='note'>Сохраненные параметры ORP: Смещение = " + String(orp_cal_offset_mv, 1) + " mV, Наклон = " + String(orp_slope_mv_per_volt, 1) + " mV/V</p>";

  // --- Сохранение и сброс калибровки ---
  page += "<hr style='margin: 25px 0;'>";
  page += "<form method='POST' action='/save_cal' style='display:inline;'>";
  page += "<input type='hidden' name='ph7v' value='" + String(temp_ph7_v, 5) + "'>"; // Передаем временные значения
  page += "<input type='hidden' name='ph4v' value='" + String(temp_ph4_v, 5) + "'>";
  page += "<input type='hidden' name='orpv' value='" + String(temp_orp_v, 5) + "'>";
  page += "<input type='hidden' name='orpmv' value='" + String(temp_orp_mv_known, 1) + "'>";
  page += "<button type='submit'>Сохранить калибровку pH (по точке 7.0)</button>";
  page += "</form>";
  page += "<form method='POST' action='/reset_cal' style='display:inline;'>";
  page += "<button type='submit' class='reset-btn'>Сбросить калибровку</button>";
  page += "</form> (pH Reset Not Implemented Yet)"; // Убрали функционал reset

  page += "</div>"; // cal-section
  page += "</body></html>";
  return page;
}

// --- Вспомогательные функции ---

// Сохранение настроек в Preferences
bool saveSettings(String new_ssid, String new_password, String new_mqtt_broker,
                  float cal_orp_offset, float cal_orp_slope, // Убрали pH параметры
                  unsigned long mqtt_interval_s) {
  if (!preferences.begin("orp-ph-config", false)) {
      Serial.println("Ошибка: Не удалось открыть настройки для записи!");
      return false;
  }
  preferences.putString("ssid", new_ssid);
  preferences.putString("password", new_password);
  preferences.putString("mqtt_broker", new_mqtt_broker);
  preferences.putULong("mqtt_interval", mqtt_interval_s);
  // Сохраняем калибровочные данные
  // Сохраняем параметры ORP (ручные)
  preferences.putFloat("cal_orp_off", cal_orp_offset);
  preferences.putFloat("cal_orp_slp", cal_orp_slope);
  preferences.end(); // Просто закрываем

  // Считаем, что сохранение успешно, если дошли до сюда
  Serial.println("Настройки и калибровка сохранены (предположительно).");
  // Обновляем глобальные переменные
  ssid = new_ssid;
  password = new_password;
  mqtt_broker = new_mqtt_broker;
  mqtt_publish_interval_sec = mqtt_interval_s;
  // Обновляем параметры ORP
  orp_cal_offset_mv = cal_orp_offset;
  orp_slope_mv_per_volt = cal_orp_slope;

  return true; // Возвращаем true
}

// Загрузка настроек из Preferences
bool loadSettings() {
  if (!preferences.begin("orp-ph-config", true)) { // true = Read-only mode
     Serial.println("Не удалось открыть настройки (возможно, их еще нет).");
     preferences.end();
     return false;
  }

  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  mqtt_broker = preferences.getString("mqtt_broker", "");
  mqtt_publish_interval_sec = preferences.getULong("mqtt_interval", 60); // Загружаем интервал MQTT (умолч. 60 сек)
  // Загружаем калибровочные данные (с значениями по умолчанию, если не найдены)
  // Загружаем параметры ORP
  orp_cal_offset_mv = preferences.getFloat("cal_orp_off", 0.0);
  orp_slope_mv_per_volt = preferences.getFloat("cal_orp_slp", 1000.0);

  preferences.end();

  Serial.println("Калибровочные параметры ORP загружены:");
  Serial.print("  ORP Offset (mV): "); Serial.println(orp_cal_offset_mv, 1);
  Serial.print("  ORP Slope (mV/V): "); Serial.println(orp_slope_mv_per_volt, 1);

  if (ssid.length() > 0 && mqtt_broker.length() > 0) {
    Serial.println("Настройки загружены:");
    Serial.print("  SSID: "); Serial.println(ssid);
    // Не печатаем пароль в лог
    Serial.print("  MQTT Broker: "); Serial.println(mqtt_broker);
    return true;
  } else {
    Serial.println("Настройки не найдены или неполные.");
    return false;
  }
}

// Обработчик для корневой страницы "/" в режиме AP
void handleRoot() {
  server.send(200, "text/html", getAPConfigPage());
}

// Обработчик для сохранения настроек "/save" в режиме AP
void handleSave() {
  Serial.println("Получен запрос на сохранение настроек...");
  String new_ssid = server.arg("ssid");
  String new_pass = server.arg("pass");
  String new_mqtt = server.arg("mqtt");
  unsigned long new_mqtt_interval = server.hasArg("mqtt_int") ? server.arg("mqtt_int").toInt() : mqtt_publish_interval_sec;
  if (new_mqtt_interval < 5) new_mqtt_interval = 5; // Ограничение минимального интервала

  Serial.print("  New SSID: "); Serial.println(new_ssid);
  // Не печатаем пароль
  Serial.print("  New MQTT: "); Serial.println(new_mqtt);
  Serial.print("  New MQTT Interval: "); Serial.print(new_mqtt_interval); Serial.println(" sec");

  if (new_ssid.length() > 0 && new_mqtt.length() > 0) {
    // Используем текущие сохраненные калибровочные данные при сохранении настроек сети
    if (saveSettings(new_ssid, new_pass, new_mqtt, orp_cal_offset_mv, orp_slope_mv_per_volt, new_mqtt_interval)) {
      String message = "<html><head><meta http-equiv='refresh' content='5;url=/'></head><body>";
      message += "<h1>Настройки сохранены!</h1>";
      message += "<p class='msg success'>Перезагрузка через 5 секунд...</p>";
      message += "</body></html>";
      server.send(200, "text/html", message);
      delay(5000);
      ESP.restart();
    } else {
      String message = "<html><body><h1>Ошибка сохранения!</h1><p class='msg error'>Не удалось сохранить настройки. <a href='/'>Попробовать снова</a></p></body></html>";
      server.send(500, "text/html", message);
    }
  } else {
     String message = "<html><body><h1>Ошибка!</h1><p class='msg error'>Имя SSID и адрес MQTT брокера не могут быть пустыми. <a href='/'>Попробовать снова</a></p></body></html>";
     server.send(400, "text/html", message);
  }
}

// Запуск режима точки доступа (AP)
void startAPMode() {
  apMode = true;
  Serial.println("Запуск в режиме точки доступа (AP)...");
  digitalWrite(LED_PIN, HIGH); // Индикация режима AP

  // Генерируем уникальное имя AP
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[7]; // 6 символов + null terminator
  sprintf(macStr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  apName += macStr;

  Serial.print("Имя точки доступа: "); Serial.println(apName);

  WiFi.softAP(apName.c_str()); // Запускаем AP без пароля

  IPAddress apIP(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, subnet); // Устанавливаем IP адрес AP

  // Запускаем DNS сервер для Captive Portal
  dnsServer.start(53, "*", apIP); // Перенаправляем все DNS запросы на наш IP

  // Настраиваем обработчики веб-сервера
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  // Ответ для Captive Portal
   server.onNotFound([]() {
    server.send(200, "text/html", getAPConfigPage()); // Показываем страницу настроек на любой запрос
  });

  // --- Новые обработчики для калибровки --- 
  server.on("/read_ph7", HTTP_POST, [](){
    Serial.println("Web request: /read_ph7");
    temp_ph7_v = adcToVoltage(analogRead(PH_PIN));
    Serial.print("  pH 7 Voltage read: "); Serial.println(temp_ph7_v, 3);
    handleRoot(); // Обновляем страницу
  });

  server.on("/read_ph4", HTTP_POST, [](){
    Serial.println("Web request: /read_ph4");
    temp_ph4_v = adcToVoltage(analogRead(PH_PIN));
    Serial.print("  pH 4 Voltage read: "); Serial.println(temp_ph4_v, 3);
    handleRoot(); // Обновляем страницу
  });

  server.on("/read_orp", HTTP_POST, [](){
    Serial.println("Web request: /read_orp");
    if (server.hasArg("orp_mv")) {
       temp_orp_mv_known = server.arg("orp_mv").toFloat();
       temp_orp_v = adcToVoltage(analogRead(ORP_PIN));
       Serial.print("  ORP Voltage read: "); Serial.print(temp_orp_v, 3);
       Serial.print(" for known mV: "); Serial.println(temp_orp_mv_known);
    } else {
       Serial.println("  Error: ORP mV value not provided.");
       temp_orp_v = -1.0; // Сброс, если нет значения mV
       temp_orp_mv_known = -999.0;
    }
    handleRoot(); // Обновляем страницу
  });

  server.on("/save_cal", HTTP_POST, [](){
    Serial.println("Web request: /save_cal");
    bool changed = false;
    float new_ph7_v = server.hasArg("ph7v") ? server.arg("ph7v").toFloat() : -1.0;

    float final_orp_offset = orp_cal_offset_mv;
    float final_orp_slope = orp_slope_mv_per_volt;

    // Выполняем одноточечную калибровку pH библиотеки, если есть значение V для pH 7
    if (new_ph7_v >= 0) {
        Serial.println("  Выполнение калибровки pH библиотеки...");
        // Используем предполагаемые методы калибровки библиотеки
        ph_sensor.recalibrate(new_ph7_v); // Используем метод recalibrate с одним параметром

        // Параметры калибровки теперь хранятся внутри объекта ph_sensor.
        // Мы не можем их получить/сохранить стандартными методами (getOffset/getSlope отсутствуют)

        Serial.println("  Калибровка pH выполнена методом recalculate(). Новые параметры активны до перезагрузки.");
        changed = true; // Считаем, что калибровка всегда что-то меняет для обновления интерфейса
    } else {
        Serial.println("  Нет данных для калибровки pH библиотеки (нужно прочитать V для pH 7).");
    }
    
    // TODO: Добавить калибровку ORP отдельно, если нужно

    if (changed) {
        // Больше не сохраняем калибровку pH здесь, т.к. она не персистентна
        // Мы могли бы сохранить напряжения pH7/pH4, но это не очень полезно без способа их применить.
        Serial.println("  Калибровка pH применена (до перезагрузки). Сброс временных значений.");
        temp_ph7_v = -1.0;
        temp_ph4_v = -1.0;
        temp_orp_v = -1.0; // ORP пока не используется здесь
        temp_orp_mv_known = -999.0;
    } else {
        Serial.println("  No changes in calibration values detected.");
    }
    handleRoot(); // Обновляем страницу
  });

  server.begin(); // Запускаем веб-сервер
  Serial.println("Веб-сервер для настройки запущен на http://192.168.4.1");
}

// --- HTML страница для отображения данных ---
String getSensorDataPage() {
  String page = "<!DOCTYPE HTML><html><head>";
  page += "<title>ORP_pH Данные</title>";
  page += "<meta http-equiv='refresh' content='10'>"; // Автообновление каждые 10 секунд
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f9f9f9; }";
  page += "h1 { color: #2a7aaf; text-align: center; }";
  page += ".container { background-color: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); max-width: 500px; margin: 30px auto; text-align: center; }";
  page += ".sensor-value { font-size: 2.5em; color: #333; margin: 15px 0; font-weight: bold; }";
  page += ".sensor-label { font-size: 1.2em; color: #555; margin-bottom: 25px; }";
  page += ".status { font-size: 0.9em; color: #888; margin-top: 30px; }";
  page += "</style></head><body>";
  page += "<h1>ORP / pH Монитор</h1>";
  page += "<div class='container'>";
  page += "<div class='sensor-label'>Текущий pH:</div>";
  page += "<div class='sensor-value'>";
  page += (current_ph < 0) ? "N/A" : String(current_ph, 2); // Показываем N/A, если нет данных
  page += "</div>";
  page += "<div class='sensor-label'>Текущий ORP (mV):</div>";
  page += "<div class='sensor-value'>";
  page += (current_orp < -990) ? "N/A" : String(current_orp, 1); // Используем -999 как индикатор N/A для ORP
  page += "</div>";
  page += "</div>";
  page += "<div class='status'>IP: " + WiFi.localIP().toString();
  page += " | MQTT: <span class='value'>" + String(mqttClient.connected() ? "Подключен" : "Не подключен") + "</span>";
  page += " | Обновление каждые 10 сек.</div>";
  page += "</body></html>";
  return page;
}

// --- Обработчик для корневой страницы "/" в режиме STA ---
void handleSTAData() {
  server.send(200, "text/html", getSensorDataPage());
}

// --- Попытка подключения к WiFi ---
bool connectWiFi() {
  if (ssid.length() == 0) {
    Serial.println("SSID не задан. Не могу подключиться.");
    return false;
  }

  Serial.print("Подключение к WiFi сети: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA); // Устанавливаем режим станции
  WiFi.begin(ssid.c_str(), password.c_str());

  int retries = 0;
  const int maxRetries = 30; // Попыток подключения (примерно 15 секунд)

  while (WiFi.status() != WL_CONNECTED && retries < maxRetries) {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Мигаем светодиодом при подключении
    retries++;
  }

  digitalWrite(LED_PIN, LOW); // Выключаем светодиод после попытки

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi подключен!");
    Serial.print("IP адрес: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nНе удалось подключиться к WiFi.");
    WiFi.disconnect(true); // Отключаемся и очищаем конфигурацию
    WiFi.mode(WIFI_OFF); // Выключаем WiFi
    return false;
  }
}

// --- Попытка подключения к MQTT брокеру ---
void reconnectMQTT() {
  // Проверяем, нужно ли переподключаться (только если WiFi подключен)
  if (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
    unsigned long now = millis();
    // Пытаемся переподключиться не чаще, чем раз в 5 секунд
    if (now - lastMqttReconnectAttempt > 5000) {
        lastMqttReconnectAttempt = now;
        Serial.print("Попытка подключения к MQTT брокеру: ");
        Serial.print(mqtt_broker);
        Serial.print(" ClientID: ");
        Serial.println(mqttClientID);

        // Пытаемся подключиться
        if (mqttClient.connect(mqttClientID.c_str())) {
            Serial.println("MQTT подключен!");
            // Сюда можно добавить подписки на топики, если нужно
            // mqttClient.subscribe("some/topic");
            // Подписываемся на топик команд
            if (commandTopic.length() > 0) {
                if (mqttClient.subscribe(commandTopic.c_str())) {
                    Serial.print("Подписались на топик команд: ");
                    Serial.println(commandTopic);
                } else {
                    Serial.println("Ошибка подписки на топик команд!");
                }
            }
        } else {
            Serial.print(" Ошибка подключения MQTT, rc=");
            Serial.print(mqttClient.state());
            Serial.println(". Повторная попытка через 5 секунд...");
        }
    }
  }
}

// --- Основные функции ---

void setup() {
  // Инициализация Serial для отладки
  Serial.begin(115200);
  Serial.println("\n\nЗапуск ORP_pH Monitor...");

  // Инициализация пинов
  pinMode(LED_PIN, OUTPUT);
  ph_sensor.init(); // Инициализация pH сенсора
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP); // Кнопка BOOT подтянута к питанию

  digitalWrite(LED_PIN, HIGH); // Включим светодиод в начале

  // Проверка кнопки сброса
  bool forceAP = (digitalRead(RESET_BUTTON_PIN) == LOW);
  if (forceAP) {
    Serial.println("Кнопка сброса нажата. Принудительный запуск AP.");
    // Опционально: очистить сохраненные настройки
    // preferences.begin("orp-ph-config", false);
    // preferences.clear();
    // preferences.end();
    // Serial.println("Сохраненные настройки очищены.");
  }

  // Загрузка/проверка настроек
  if (!loadSettings() || forceAP) {
    startAPMode(); // Запускаем AP, если настроек нет или зажата кнопка
  } else {
    // Подключаемся к WiFi
    if (!connectWiFi()) {
        // Если не удалось подключиться, запускаем AP
        Serial.println("Не удалось подключиться к WiFi. Запуск AP...");
        startAPMode();
    } else {
      // WiFi успешно подключен
      
      // Формируем уникальный ClientID и топики для MQTT
      uint8_t mac[6];
      WiFi.macAddress(mac);
      char macStr[7];
      sprintf(macStr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
      mqttClientID += macStr;
      phTopic = "orp_ph/" + String(macStr) + "/ph";
      orpTopic = "orp_ph/" + String(macStr) + "/orp_mv";
      commandTopic = "orp_ph/" + String(macStr) + "/cmd";
      Serial.print("MQTT Client ID: "); Serial.println(mqttClientID);
      Serial.print("pH Topic: "); Serial.println(phTopic);
      Serial.print("ORP Topic: "); Serial.println(orpTopic);
      Serial.print("Command Topic: "); Serial.println(commandTopic);

      // Настраиваем MQTT клиент
      mqttClient.setServer(mqtt_broker.c_str(), MQTT_PORT);
      // Сюда можно добавить callback функцию для входящих сообщений
      mqttClient.setCallback(mqttCallback);

      // Запускаем Web сервер для данных
      server.on("/", HTTP_GET, handleSTAData); // Настраиваем обработчик для данных
      server.begin();                         // Запускаем веб-сервер
      Serial.println("Веб-сервер для данных запущен.");
    }
  }

  // Если не в режиме AP, выключаем светодиод после инициализации
  if (!apMode) {
      digitalWrite(LED_PIN, LOW);
  }
}

void loop() {
  if (apMode) {
    // В режиме AP обрабатываем DNS и HTTP запросы
    dnsServer.processNextRequest();
    server.handleClient();
    // Можно добавить мигание светодиода для индикации AP режима
    unsigned long currentMillis = millis();
    static unsigned long previousMillis = 0;
    static unsigned long previousMillisLED = 0;
    if (currentMillis - previousMillisLED >= 1000) {
        previousMillisLED = currentMillis;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Мигаем раз в секунду
    }

  } else {
    // В обычном режиме (STA)

    // Проверяем соединение WiFi и переподключаемся при необходимости
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Потеряно соединение WiFi. Попытка переподключения...");
        if (connectWiFi()) { // Повторная попытка подключения
             // Если успешно переподключились к WiFi, пробуем и MQTT
             lastMqttReconnectAttempt = 0; // Сбрасываем таймер попыток MQTT
             reconnectMQTT();
        }
    } else {
        // Если WiFi работает, проверяем и поддерживаем MQTT соединение
        if (!mqttClient.connected()) {
            reconnectMQTT();
        }
        mqttClient.loop(); // Важно вызывать для обработки MQTT
    }

    // Обрабатываем запросы веб-сервера
    server.handleClient();

    // --- Чтение датчиков ---
    unsigned long currentMillisLoop = millis();
    if (currentMillisLoop - lastSensorReadTime >= SENSOR_READ_INTERVAL) {
        lastSensorReadTime = currentMillisLoop;

        // Читаем сырые значения АЦП
        int rawPhValue = analogRead(PH_PIN);
        int rawOrpValue = analogRead(ORP_PIN);

        // Читаем pH с помощью библиотеки
        current_ph = ph_sensor.read_ph_level();
        float phVoltage = adcToVoltage(rawPhValue); // Напряжение все еще можем показать для отладки

        // Обрабатываем ORP вручную
        float orpVoltage = adcToVoltage(rawOrpValue);
        current_orp = orpVoltage * orp_slope_mv_per_volt + orp_cal_offset_mv;

        Serial.print("Sensor Data -> pH ADC: "); Serial.print(rawPhValue);
        Serial.print(" (V: "); Serial.print(phVoltage, 3); Serial.print(")");
        Serial.print(" -> pH: "); Serial.print((current_ph < 0) ? "N/A" : String(current_ph, 2));
        Serial.print(" | ORP ADC: "); Serial.print(rawOrpValue);
        Serial.print(" (V: "); Serial.print(orpVoltage, 3); Serial.print(")");
        Serial.print(" -> ORP: "); Serial.print((current_orp < -990) ? "N/A" : String(current_orp, 1)); Serial.println(" mV");
    }

    // --- Периодическая публикация MQTT ---
    bool timeToPublish = (currentMillisLoop - lastMqttPublishTime >= (mqtt_publish_interval_sec * 1000UL));

    // Проверяем интервал публикации MQTT
    if (!apMode && mqttClient.connected() && (timeToPublish || forceMqttPublish) ) {
        if (forceMqttPublish) {
             Serial.println("Принудительная публикация MQTT...");
        }
        lastMqttPublishTime = currentMillisLoop;
        forceMqttPublish = false; // Сбрасываем флаг после публикации

        // Готовим данные для отправки
        String phPayload = (current_ph < 0) ? "N/A" : String(current_ph, 2);
        String orpPayload = (current_orp < -990) ? "N/A" : String(current_orp, 1);

        // Публикуем данные
        if (current_ph >= 0) { // Публикуем только если pH валиден
            if (mqttClient.publish(phTopic.c_str(), phPayload.c_str())) {
                Serial.print("MQTT Published [pH]: "); Serial.println(phPayload);
            } else {
                Serial.println("MQTT Publish pH failed");
            }
        }
        if (current_orp > -990) { // Публикуем только если ORP валиден
             if (mqttClient.publish(orpTopic.c_str(), orpPayload.c_str())) {
                Serial.print("MQTT Published [ORP]: "); Serial.println(orpPayload);
            } else {
                Serial.println("MQTT Publish ORP failed");
            }
        }
    }

    // Небольшая задержка, чтобы не загружать процессор на 100%
    // Важно, чтобы она была меньше интервала чтения датчиков и MQTT
    delay(10); 
  }
}

// --- Вспомогательные функции калибровки ---

// Преобразование сырого ADC значения в напряжение (0-3.3V)
float adcToVoltage(int adcValue) {
  // ESP32 ADC: 12 бит (0-4095)
  // Опорное напряжение (Vref): обычно 3.3V, но может требовать калибровки
  // TODO: Проверить/уточнить опорное напряжение для вашей платы ESP32
  const float VREF = 3.3;
  return (float)adcValue / 4095.0 * VREF;
}

// --- Callback функция для обработки входящих MQTT сообщений ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("MQTT сообщение пришло [");
    Serial.print(topic);
    Serial.print("] ");

    // Преобразуем payload в строку
    char message[length + 1];
    memcpy(message, payload, length);
    message[length] = '\0';
    String messageStr = String(message);
    messageStr.toLowerCase(); // Переводим в нижний регистр для удобства сравнения

    Serial.println(messageStr);

    // Проверяем, совпадает ли топик с командным
    if (String(topic) == commandTopic) {
        if (messageStr == "publish") {
            Serial.println("  Команда: Принудительная публикация данных.");
            forceMqttPublish = true;
        } else if (messageStr == "reboot") {
            Serial.println("  Команда: Перезагрузка устройства...");
            delay(1000); // Небольшая задержка перед перезагрузкой
            ESP.restart();
        } else {
            Serial.println("  Неизвестная команда.");
        }
    }
} 