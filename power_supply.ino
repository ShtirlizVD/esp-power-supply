/*
 * Лабораторный БП с автоматическим переключением обмоток
 * 
 * Аппаратная часть:
 *   - ESP8266 (Wemos D1 Mini) - контроллер
 *   - PCF8575 (адрес 0x20) - расширитель портов
 *   - INA226 (адрес 0x40) - датчик тока/напряжения
 *   - Силовое реле Hella 4RA 003-510-73 (контактор) - на общий провод
 *   - 3 реле обмоток трансформатора (6V, 14V, 20V; 28V = все выкл)
 *   - 40N03P - управляет контактором
 *   - 2N7002 x3 - управляют реле обмоток
 * 
 * Управление:
 *   P00 - контактор (40N03P)         HIGH = замкнут (ток идёт)
 *   P01 - реле обмотки 1 (6V)        HIGH = вкл
 *   P02 - реле обмотки 2 (14V)       HIGH = вкл
 *   P03 - реле обмотки 3 (20V)       HIGH = вкл
 *                                    (все LOW = обмотка 4, 28V)
 * 
 * Переключение обмоток:
 *   1. Размыкаем контактор (ток = 0, нет дуги)
 *   2. Переключаем реле обмоток без нагрузки
 *   3. Замыкаем контактор обратно
 */

#include <Wire.h>
#include "PCF8575.h"
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include "GyverINA.h"

// ============= АППАРАТНАЯ КОНФИГУРАЦИЯ =============

// PCF8575
PCF8575 pcf(0x20);
#define SDA_PIN 0   // GPIO0 = D3
#define SCL_PIN 2   // GPIO2 = D4

// Назначение пинов PCF8575 (используются числа 0-15, не P00-P15)
#define CONTACTOR  0   // Силовое реле Hella (40N03P)
#define RELAY1     1   // Обмотка 1 - 6V  (2N7002)
#define RELAY2     2   // Обмотка 2 - 14V (2N7002)
#define RELAY3     3   // Обмотка 3 - 20V (2N7002)
                        // Все реле выкл = обмотка 4 (28V)

// INA226
INA226 ina(0.002, 5.0);  // шунт 0.002Ω, макс ток 5A

// Безопасное чтение напряжения: если INA226 не отвечает или мусор - вернём 0
float safeGetVoltage() {
  float v = ina.getVoltage();
  if (v < 0 || v > 36.0) return 0.0;  // INA226 max = 36V
  return v;
}

float safeGetCurrent() {
  float i = ina.getCurrent();
  if (i < -10.0 || i > 10.0) return 0.0;  // запас по диапазону
  return i;
}

float safeGetPower() {
  float p = ina.getPower();
  if (p < 0 || p > 200.0) return 0.0;
  return p;
}

// ============= ПАРАМЕТРЫ ОБМОТОК =============

const float tapVoltageAC[4] = {6.38, 14.25, 20.6, 28.8};
const float tapVoltageDC[4] = {8.5, 19.0, 27.5, 38.5};  // AC × 1.3-1.4 после моста

const String tapNames[4] = {
  "1 (6.4V AC)",
  "2 (14.3V AC)",
  "3 (20.6V AC)",
  "4 (28.8V AC)"
};

// ============= НАСТРОЙКИ WIFI И OTA =============

const char* ssid     = "PowerSupply";
const char* password = "12345678";

// Логин/пароль для OTA-обновлений через /update
const char* otaUser     = "admin";
const char* otaPassword = "1234";

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ============= ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ =============

int   currentTap   = 3;     // текущая активная обмотка
float setVoltage   = 0;     // заданное напряжение (если есть ADC)
float setCurrent   = 0;     // заданный ток
bool  outputEnabled = true; // состояние выхода

unsigned long lastSwitchTime = 0;
const unsigned long switchCooldown = 1000;  // мин 1 сек между переключениями

// ============= УПРАВЛЕНИЕ РЕЛЕ =============

void allRelaysOff() {
  pcf.write(RELAY1, LOW);
  pcf.write(RELAY2, LOW);
  pcf.write(RELAY3, LOW);
}

// Переключение обмотки с защитой от дуги через контактор
void switchTap(int tap) {
  if (tap == currentTap) return;
  if (tap < 0 || tap > 3) return;
  
  // Защита от частых переключений
  unsigned long now = millis();
  if (now - lastSwitchTime < switchCooldown) return;
  
  Serial.printf("[TAP] Переключение: %s -> обмотка %d (%.2fV AC)\n",
                tapNames[currentTap].c_str(), tap + 1, tapVoltageAC[tap]);
  
  // 1. Размыкаем контактор - ток через реле обмоток = 0
  pcf.write(CONTACTOR, LOW);
  delay(100);  // время на размыкание контактов Hella
  
  // 2. Выключаем все реле обмоток
  allRelaysOff();
  delay(50);   // время на размыкание контактов реле
  
  // 3. Включаем нужное реле обмотки
  switch (tap) {
    case 0: pcf.write(RELAY1, HIGH); break;  // 6V
    case 1: pcf.write(RELAY2, HIGH); break;  // 14V
    case 2: pcf.write(RELAY3, HIGH); break;  // 20V
    case 3: break;                            // 28V - все реле выкл
  }
  delay(50);   // время на замыкание контактов реле
  
  // 4. Замыкаем контактор - подаём ток
  pcf.write(CONTACTOR, HIGH);
  
  currentTap = tap;
  lastSwitchTime = now;
  
  Serial.printf("[TAP] Готово. Активна обмотка %d\n", tap + 1);
}

// ============= ЛОГИКА ВЫБОРА ОБМОТКИ =============

// Выбор оптимальной обмотки для заданного напряжения
// Запас ~3V на регулирование и падение на проходном транзисторе
int getOptimalTapForVoltage(float targetV) {
  if (targetV <= 5.0)  return 0;  // 6V  обмотка
  if (targetV <= 15.0) return 1;  // 14V обмотка
  if (targetV <= 23.0) return 2;  // 20V обмотка
  return 3;                        // 28V обмотка
}

// Автоматическая оптимизация обмотки по фактическому выходному напряжению
// Переключаемся ВНИЗ если можем - меньше нагрев проходного транзистора
void autoSwitchTap() {
  if (!outputEnabled) return;
  
  float busV = safeGetVoltage();
  int optimal = getOptimalTapForVoltage(busV + 2.0);  // +2V запас
  
  if (optimal != currentTap) {
    // Только если переключение безопасно (вниз - всегда можно,
    // вверх - только если выходное напряжение близко к пределу текущей обмотки)
    if (optimal < currentTap) {
      // Переключаемся вниз - безопасно
      switchTap(optimal);
    } else if (busV > tapVoltageDC[currentTap] - 3.0) {
      // Переключаемся вверх - только если не хватает напряжения
      switchTap(optimal);
    }
  }
}

// ============= ВЕБ-ИНТЕРФЕЙС =============

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Лабораторный БП</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: Arial, sans-serif;
      max-width: 480px;
      margin: 20px auto;
      padding: 10px;
      background: #f5f5f5;
    }
    h2 { text-align: center; color: #333; }
    .gauge {
      font-size: 42px;
      text-align: center;
      margin: 10px 0;
      font-weight: bold;
      color: #2196F3;
      background: #fff;
      padding: 15px;
      border-radius: 8px;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    .gauge.current { color: #FF9800; }
    .label {
      font-size: 13px;
      color: #666;
      text-align: center;
      margin-bottom: 5px;
    }
    .tap-info {
      background: #4CAF50;
      color: white;
      padding: 12px;
      text-align: center;
      border-radius: 6px;
      margin: 10px 0;
      font-size: 16px;
    }
    .power-info {
      background: #fff;
      padding: 10px;
      border-radius: 6px;
      margin: 10px 0;
      text-align: center;
      font-size: 14px;
      color: #555;
    }
    button {
      width: 100%;
      padding: 15px;
      margin: 5px 0;
      font-size: 16px;
      border: none;
      border-radius: 6px;
      cursor: pointer;
      color: white;
    }
    .btn-on { background: #4CAF50; }
    .btn-off { background: #f44336; }
    .btn-test { background: #FF9800; }
    .btn-update { background: #607D8B; font-size: 14px; padding: 10px; }
    hr { border: none; border-top: 1px solid #ddd; margin: 15px 0; }
    a { color: #2196F3; text-decoration: none; }
  </style>
</head>
<body>
  <h2>Лабораторный БП</h2>
  
  <div class="label">Выходное напряжение</div>
  <div class="gauge" id="voltage">--.-- V</div>
  
  <div class="label">Выходной ток</div>
  <div class="gauge current" id="current">--.-- A</div>
  
  <div class="power-info">
    Мощность: <span id="power">0.00 W</span>
  </div>
  
  <div class="tap-info">
    Активная обмотка: <span id="tap">--</span>
  </div>
  
  <hr>
  
  <button class="btn-off" onclick="outputToggle()" id="outBtn">ВЫКЛ выход</button>
  
  <hr>
  
  <div class="label">Тест силового реле Hella (независимо от выхода)</div>
  <button class="btn-test" onclick="testRelay()" id="testBtn">ТЕСТ Hella: ВКЛ</button>
  
  <hr>
  
  <button class="btn-update" onclick="location.href='/update'">OTA обновление прошивки</button>
  
  <script>
    function update() {
      fetch('/status')
        .then(r => r.json())
        .then(d => {
          document.getElementById('voltage').textContent = d.voltage.toFixed(2) + ' V';
          document.getElementById('current').textContent = d.current.toFixed(3) + ' A';
          document.getElementById('power').textContent = d.power.toFixed(2) + ' W';
          document.getElementById('tap').textContent = d.tapName;
          
          var btn = document.getElementById('outBtn');
          if (d.output) {
            btn.textContent = 'ВЫКЛ выход';
            btn.className = 'btn-off';
          } else {
            btn.textContent = 'ВКЛ выход';
            btn.className = 'btn-on';
          }
        })
        .catch(err => console.error(err));
    }
    
    function outputToggle() {
      fetch('/toggle').then(() => update());
    }
    
    function testRelay() {
      fetch('/test_relay')
        .then(r => r.text())
        .then(state => {
          var btn = document.getElementById('testBtn');
          btn.textContent = 'ТЕСТ Hella: ' + (state === 'ON' ? 'ВЫКЛ' : 'ВКЛ');
        });
    }
    
    setInterval(update, 500);
    update();
  </script>
</body>
</html>
)rawliteral";
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "text/html", html);
}

void handleStatus() {
  float voltage = safeGetVoltage();
  float current = safeGetCurrent();
  float power   = safeGetPower();
  
  String json = "{";
  json += "\"voltage\":" + String(voltage, 2) + ",";
  json += "\"current\":" + String(current, 3) + ",";
  json += "\"power\":"   + String(power, 2)   + ",";
  json += "\"tap\":"     + String(currentTap) + ",";
  json += "\"tapName\":\"" + tapNames[currentTap] + "\",";
  json += "\"output\":" + String(outputEnabled ? "true" : "false");
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleToggle() {
  outputEnabled = !outputEnabled;
  
  if (outputEnabled) {
    // Включаем - замыкаем контактор
    pcf.write(CONTACTOR, HIGH);
    Serial.println("[OUT] Выход включён");
  } else {
    // Выключаем - размыкаем контактор
    pcf.write(CONTACTOR, LOW);
    Serial.println("[OUT] Выход выключен");
  }
  
  server.send(200, "text/plain", "OK");
}

// Ручное переключение обмотки: /tap?n=2
void handleSetTap() {
  if (server.hasArg("n")) {
    int tap = server.arg("n").toInt();
    if (tap >= 0 && tap <= 3) {
      switchTap(tap);
      server.send(200, "text/plain", "OK tap=" + String(tap));
      return;
    }
  }
  server.send(400, "text/plain", "Bad request");
}

// Тест силового реле Hella: /test_relay
// Просто переключает контактор без влияния на логику обмоток
bool testRelayState = false;
void handleTestRelay() {
  testRelayState = !testRelayState;
  pcf.write(CONTACTOR, testRelayState ? HIGH : LOW);
  Serial.printf("[TEST] Hella: %s\n", testRelayState ? "ON" : "OFF");
  server.send(200, "text/plain", testRelayState ? "ON" : "OFF");
}

// ============= SETUP =============

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== Лабораторный БП ===");
  
  // I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  Serial.println("[I2C] Инициализация OK");
  
  // PCF8575
  pcf.begin();
  // Стартовое состояние: всё выключено
  pcf.write(CONTACTOR, LOW);  // контактор разомкнут
  allRelaysOff();
  Serial.println("[PCF] Расширитель портов инициализирован");
  
  // INA226
  ina.begin();
  ina.setAveraging(INA226_AVG_X64);      // усреднение для стабильности
  Serial.println("[INA] Датчик тока инициализирован");
  
  // WiFi - точка доступа
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  Serial.print("[WiFi] Точка доступа: ");
  Serial.println(ssid);
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.softAPIP());  // 192.168.4.1
  
  // Веб-сервер
  server.on("/",          handleRoot);
  server.on("/status",    handleStatus);
  server.on("/toggle",    handleToggle);
  server.on("/tap",       handleSetTap);
  server.on("/test_relay", handleTestRelay);
  
  // OTA-обновление на /update
  httpUpdater.setup(&server, "/update", otaUser, otaPassword);
  
  server.begin();
  Serial.println("[HTTP] Сервер запущен на порту 80");
  Serial.println("[OTA]  Обновление на http://192.168.4.1/update");
  Serial.println("       Логин: " + String(otaUser) + " / Пароль: " + String(otaPassword));
  
  // Старт на 4-й обмотке (28V) - покрывает весь диапазон
  Serial.println("[INIT] Старт на максимальной обмотке (28V)");
  switchTap(3);
  
  Serial.println("=== Готово ===\n");
}

// ============= LOOP =============

void loop() {
  server.handleClient();
  
  // Автопереключение обмоток каждые 500мс
  static unsigned long lastAutoSwitch = 0;
  if (millis() - lastAutoSwitch > 500) {
    autoSwitchTap();
    lastAutoSwitch = millis();
  }
}
