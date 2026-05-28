#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "time.h"

// =====================================================
// WiFi
// =====================================================
const char* WIFI_SSID = "Jorge Esteban's S25";
const char* WIFI_PASSWORD = "thank u next";


const char* MQTT_BROKER = "broker.hivemq.com";
const int MQTT_PORT = 8883;


const char* MQTT_USER = "";
const char* MQTT_PASSWORD = "";

const char* DEVICE_ID = "comfortsense-esp32-001";

const char* TOPIC_TELEMETRY = "comfortsense/esp32-001/telemetry";
const char* TOPIC_STATUS    = "comfortsense/esp32-001/status";
const char* TOPIC_COMMANDS  = "comfortsense/esp32-001/cmd";

// Publicar cada 5 segundos
unsigned long publishIntervalMs = 5000;
unsigned long lastPublishMs = 0;
unsigned long lastMqttReconnectAttempt = 0;

// =====================================================
// NTP - Colombia UTC-5
// =====================================================
const long GMT_OFFSET_SEC = -5 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

// =====================================================
// Pines I2C - BME280 + BH1750
// =====================================================
#define SDA_PIN 21
#define SCL_PIN 22

// =====================================================
// Sensor de sonido
// =====================================================
const int PIN_AUDIO = 34;   // AO del módulo de sonido
const int PIN_DO = 27;      // DO opcional

const int ADC_MAX = 4095;
const float VREF = 3.3;
const int SAMPLE_WINDOW_MS = 100;

// =====================================================
// Objetos
// =====================================================
Adafruit_BME280 bme;
BH1750 lightMeter;

bool bmeReady = false;
bool lightReady = false;

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);
WebServer server(80);

String lastTelemetryPayload = "{}";

// =====================================================
// Funciones auxiliares de scoring
// =====================================================
float clampValue(float value, float minValue, float maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

float scoreRange(float value, float idealMin, float idealMax, float absoluteMin, float absoluteMax) {
  if (value >= idealMin && value <= idealMax) {
    return 100.0;
  }

  if (value < idealMin) {
    if (value <= absoluteMin) return 0.0;
    float score = 100.0 * (value - absoluteMin) / (idealMin - absoluteMin);
    return clampValue(score, 0.0, 100.0);
  }

  if (value > idealMax) {
    if (value >= absoluteMax) return 0.0;
    float score = 100.0 * (absoluteMax - value) / (absoluteMax - idealMax);
    return clampValue(score, 0.0, 100.0);
  }

  return 0.0;
}

float scoreInverse(float value, float minValue, float maxValue) {
  value = clampValue(value, minValue, maxValue);
  float score = 100.0 * (maxValue - value) / (maxValue - minValue);
  return clampValue(score, 0.0, 100.0);
}

// =====================================================
// Lectura ruido
// =====================================================
int readNoisePeakToPeak() {
  unsigned long startMillis = millis();

  int signalMax = 0;
  int signalMin = ADC_MAX;

  while (millis() - startMillis < SAMPLE_WINDOW_MS) {
    int lectura = analogRead(PIN_AUDIO);

    if (lectura > signalMax) signalMax = lectura;
    if (lectura < signalMin) signalMin = lectura;

    delayMicroseconds(200);
  }

  return signalMax - signalMin;
}

// =====================================================
// Puntajes individuales
// =====================================================
float scoreTemperature(float temperature) {
  return scoreRange(temperature, 20.0, 26.0, 16.0, 32.0);
}

float scoreHumidity(float humidity) {
  return scoreRange(humidity, 40.0, 60.0, 25.0, 90.0);
}

float scoreLight(float lux) {
  return scoreRange(lux, 300.0, 750.0, 50.0, 1200.0);
}

float scoreNoise(int peakToPeak) {
  const int NOISE_QUIET = 40;
  const int NOISE_LOUD = 1000;

  return scoreInverse(peakToPeak, NOISE_QUIET, NOISE_LOUD);
}

// =====================================================
// Índice de confort
// =====================================================
float calculateComfortIndex(
  float temperatureScore,
  float humidityScore,
  float lightScore,
  float noiseScore
) {
  return (0.25 * temperatureScore) +
         (0.10 * humidityScore) +
         (0.30 * lightScore) +
         (0.35 * noiseScore);
}

String getComfortStatus(float index) {
  if (index >= 70.0) return "Confortable";
  if (index >= 40.0) return "Aceptable";
  return "Poco confortable";
}

// =====================================================
// JSON helpers
// =====================================================
String jsonNumberOrNull(float value, int decimals) {
  if (isnan(value)) return "null";
  return String(value, decimals);
}

String getTimestampISO() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "time_not_available";
  }

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S-05:00", &timeinfo);
  return String(buffer);
}

// =====================================================
// WiFi
// =====================================================
void connectWiFi() {
  Serial.println();
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi conectado.");
  Serial.print("IP local: ");
  Serial.println(WiFi.localIP());
}

// =====================================================
// NTP
// =====================================================
void syncTimeNTP() {
  Serial.println("Sincronizando tiempo por NTP...");

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);

  struct tm timeinfo;
  int attempts = 0;

  while (!getLocalTime(&timeinfo) && attempts < 20) {
    Serial.print(".");
    delay(500);
    attempts++;
  }

  Serial.println();

  if (attempts < 20) {
    Serial.print("Tiempo sincronizado: ");
    Serial.println(getTimestampISO());
  } else {
    Serial.println("No se pudo sincronizar NTP todavía.");
  }
}

// =====================================================
// Payload status
// =====================================================
String buildStatusPayload(const char* state) {
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"state\":\"" + String(state) + "\",";
  payload += "\"timestamp\":\"" + getTimestampISO() + "\",";
  payload += "\"uptime_ms\":" + String(millis()) + ",";
  payload += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  payload += "\"mqtt_connected\":" + String(mqttClient.connected() ? "true" : "false") + ",";
  payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"flash_size\":" + String(ESP.getFlashChipSize());
  payload += "}";

  return payload;
}

// =====================================================
// MQTT callback
// =====================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Mensaje recibido en topic ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(message);

  if (message == "ping") {
    String statusPayload = buildStatusPayload("pong");
    mqttClient.publish(TOPIC_STATUS, statusPayload.c_str(), true);
  }

  if (message.startsWith("interval:")) {
    unsigned long newInterval = message.substring(9).toInt();

    if (newInterval >= 1000 && newInterval <= 60000) {
      publishIntervalMs = newInterval;

      String statusPayload = buildStatusPayload("interval_updated");
      mqttClient.publish(TOPIC_STATUS, statusPayload.c_str(), true);

      Serial.print("Nuevo intervalo de publicación: ");
      Serial.println(publishIntervalMs);
    }
  }
}

// =====================================================
// MQTT connect
// =====================================================
void ensureMqttConnected() {
  if (mqttClient.connected()) return;

  unsigned long now = millis();

  if (now - lastMqttReconnectAttempt < 5000) {
    return;
  }

  lastMqttReconnectAttempt = now;

  Serial.print("Conectando a MQTT TLS... ");

  uint64_t chipId = ESP.getEfuseMac();
  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)(chipId & 0xFFFFFFFF), HEX);

  String offlinePayload = buildStatusPayload("offline");

  bool connected = false;

  if (strlen(MQTT_USER) > 0) {
    connected = mqttClient.connect(
      clientId.c_str(),
      MQTT_USER,
      MQTT_PASSWORD,
      TOPIC_STATUS,
      1,
      true,
      offlinePayload.c_str()
    );
  } else {
    connected = mqttClient.connect(
      clientId.c_str(),
      TOPIC_STATUS,
      1,
      true,
      offlinePayload.c_str()
    );
  }

  if (connected) {
    Serial.println("conectado.");

    String onlinePayload = buildStatusPayload("online");
    mqttClient.publish(TOPIC_STATUS, onlinePayload.c_str(), true);

    mqttClient.subscribe(TOPIC_COMMANDS);

    Serial.print("Suscrito a: ");
    Serial.println(TOPIC_COMMANDS);
  } else {
    Serial.print("falló. Estado MQTT: ");
    Serial.println(mqttClient.state());
  }
}

// =====================================================
// Construir telemetría
// =====================================================
String buildTelemetryPayload() {
  float temperature = NAN;
  float humidity = NAN;
  float pressure = NAN;
  float lux = NAN;

  float temperatureScore = 0.0;
  float humidityScore = 0.0;
  float lightScore = 0.0;
  float noiseScore = 0.0;

  bool bmeAvailable = false;
  bool lightAvailable = false;

  Serial.println();
  Serial.println("Lectura de sensores:");

  // =====================================================
  // Sensor 1: BME280
  // =====================================================
  if (bmeReady) {
    temperature = bme.readTemperature();
    humidity = bme.readHumidity();
    pressure = bme.readPressure() / 100.0F;

    if (!isnan(temperature) && !isnan(humidity) && !isnan(pressure)) {
      bmeAvailable = true;
      temperatureScore = scoreTemperature(temperature);
      humidityScore = scoreHumidity(humidity);
    }

    Serial.print("Temperatura: ");
    Serial.print(temperature);
    Serial.print(" C | Score temp: ");
    Serial.println(temperatureScore);

    Serial.print("Humedad: ");
    Serial.print(humidity);
    Serial.print(" % | Score humedad: ");
    Serial.println(humidityScore);

    Serial.print("Presion: ");
    Serial.print(pressure);
    Serial.println(" hPa");
  } else {
    Serial.println("BME280 no disponible.");
  }

  // =====================================================
  // Sensor 2: BH1750
  // =====================================================
  if (lightReady) {
    lux = lightMeter.readLightLevel();

    if (!isnan(lux) && lux >= 0) {
      lightAvailable = true;
      lightScore = scoreLight(lux);
    }

    Serial.print("Iluminacion: ");
    Serial.print(lux);
    Serial.print(" lux | Score luz: ");
    Serial.println(lightScore);
  } else {
    Serial.println("BH1750 no disponible.");
  }

  // =====================================================
  // Sensor 3: Módulo de ruido
  // =====================================================
  int noisePeakToPeak = readNoisePeakToPeak();
  int noiseDigital = digitalRead(PIN_DO);

  float noiseVoltagePP = (noisePeakToPeak * VREF) / ADC_MAX;
  float noisePercent = (noisePeakToPeak * 100.0) / ADC_MAX;

  noiseScore = scoreNoise(noisePeakToPeak);

  Serial.print("Ruido pico a pico ADC: ");
  Serial.print(noisePeakToPeak);
  Serial.print(" | Voltaje pp: ");
  Serial.print(noiseVoltagePP, 3);
  Serial.print(" V | Ruido relativo: ");
  Serial.print(noisePercent, 2);
  Serial.print(" % | Score ruido: ");
  Serial.println(noiseScore);

  Serial.print("Salida digital DO ruido: ");
  Serial.println(noiseDigital);

  // =====================================================
  // Índice de confort
  // =====================================================
  float comfortIndex = calculateComfortIndex(
    temperatureScore,
    humidityScore,
    lightScore,
    noiseScore
  );

  String comfortStatus = getComfortStatus(comfortIndex);

  // =====================================================
  // JSON final
  // =====================================================
  String payload = "{";

  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"timestamp\":\"" + getTimestampISO() + "\",";
  payload += "\"uptime_ms\":" + String(millis()) + ",";

  payload += "\"sensors\":{";

  payload += "\"bme280_available\":" + String(bmeAvailable ? "true" : "false") + ",";
  payload += "\"temperature_c\":" + jsonNumberOrNull(temperature, 2) + ",";
  payload += "\"humidity_percent\":" + jsonNumberOrNull(humidity, 2) + ",";
  payload += "\"pressure_hpa\":" + jsonNumberOrNull(pressure, 2) + ",";

  payload += "\"bh1750_available\":" + String(lightAvailable ? "true" : "false") + ",";
  payload += "\"light_lux\":" + jsonNumberOrNull(lux, 2) + ",";

  payload += "\"noise_peak_to_peak_adc\":" + String(noisePeakToPeak) + ",";
  payload += "\"noise_voltage_pp\":" + String(noiseVoltagePP, 3) + ",";
  payload += "\"noise_relative_percent\":" + String(noisePercent, 2) + ",";
  payload += "\"noise_digital_do\":" + String(noiseDigital);

  payload += "},";

  payload += "\"scores\":{";
  payload += "\"temperature\":" + String(temperatureScore, 2) + ",";
  payload += "\"humidity\":" + String(humidityScore, 2) + ",";
  payload += "\"light\":" + String(lightScore, 2) + ",";
  payload += "\"noise\":" + String(noiseScore, 2);
  payload += "},";

  payload += "\"comfort\":{";
  payload += "\"index\":" + String(comfortIndex, 2) + ",";
  payload += "\"status\":\"" + comfortStatus + "\"";
  payload += "},";

  payload += "\"connectivity\":{";
  payload += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"mqtt_connected\":" + String(mqttClient.connected() ? "true" : "false");
  payload += "}";

  payload += "}";

  Serial.println();
  Serial.println("===== RESULTADO COMFORTSENSE =====");
  Serial.print("Indice de confort: ");
  Serial.print(comfortIndex);
  Serial.print(" / 100 | Estado: ");
  Serial.println(comfortStatus);
  Serial.println("Payload MQTT:");
  Serial.println(payload);
  Serial.println("==================================");

  return payload;
}

// =====================================================
// Publicar MQTT
// =====================================================
void publishTelemetry() {
  if (!mqttClient.connected()) {
    Serial.println("MQTT no conectado. No se publica telemetría.");
    return;
  }

  String payload = buildTelemetryPayload();
  lastTelemetryPayload = payload;

  bool published = mqttClient.publish(TOPIC_TELEMETRY, payload.c_str());

  if (published) {
    Serial.print("Publicado en topic: ");
    Serial.println(TOPIC_TELEMETRY);
  } else {
    Serial.println("ERROR: No se pudo publicar. Puede ser tamaño del payload o conexión MQTT.");
  }
}

// =====================================================
// Healthcheck HTTP
// =====================================================
void handleHealth() {
  String response = "{";
  response += "\"status\":\"ok\",";
  response += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  response += "\"timestamp\":\"" + getTimestampISO() + "\",";
  response += "\"uptime_ms\":" + String(millis()) + ",";
  response += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  response += "\"mqtt_connected\":" + String(mqttClient.connected() ? "true" : "false") + ",";
  response += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  response += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  response += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  response += "\"flash_size\":" + String(ESP.getFlashChipSize()) + ",";
  response += "\"publish_interval_ms\":" + String(publishIntervalMs) + ",";
  response += "\"last_telemetry\":" + lastTelemetryPayload;
  response += "}";

  server.send(200, "application/json", response);
}

void handleRoot() {
  server.send(
    200,
    "text/plain",
    "ComfortSense-IoT ESP32 activo. Usa /health para verificar estado."
  );
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/health", HTTP_GET, handleHealth);
  server.begin();

  Serial.println("Servidor HTTP iniciado.");
  Serial.print("Healthcheck: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/health");
}

// =====================================================
// Setup
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Iniciando ComfortSense-IoT con MQTT TLS + NTP + Healthcheck + 3 sensores...");

  // =====================================================
  // I2C para BME280 y BH1750
  // =====================================================
  Wire.begin(SDA_PIN, SCL_PIN);

  // =====================================================
  // Configuración del sensor de ruido
  // =====================================================
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_AUDIO, ADC_11db);
  pinMode(PIN_DO, INPUT);

  // =====================================================
  // Iniciar BME280
  // =====================================================
  if (bme.begin(0x76)) {
    Serial.println("BME280 detectado en direccion 0x76");
    bmeReady = true;
  } else if (bme.begin(0x77)) {
    Serial.println("BME280 detectado en direccion 0x77");
    bmeReady = true;
  } else {
    Serial.println("ERROR: No se detecto el BME280.");
    Serial.println("Revisa VCC, GND, SDA, SCL, CSB y SDO.");
  }

  // =====================================================
  // Iniciar BH1750
  // =====================================================
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 detectado correctamente.");
    lightReady = true;
  } else {
    Serial.println("ERROR: No se detecto el BH1750.");
    Serial.println("Revisa VCC, GND, SDA, SCL y ADD.");
  }

  // =====================================================
  // WiFi + NTP + WebServer
  // =====================================================
  connectWiFi();
  syncTimeNTP();

  setupWebServer();

  // =====================================================
  // TLS 
  // =====================================================
  const char* cert PROGMEM = R"Cert(-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)Cert";

  secureClient.setCACert(cert);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

 
  mqttClient.setBufferSize(1024);

  ensureMqttConnected();

  Serial.println("--------------------------------------");
}

// =====================================================
// Loop
// =====================================================
void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    syncTimeNTP();
  }

  ensureMqttConnected();
  mqttClient.loop();

  unsigned long now = millis();

  if (now - lastPublishMs >= publishIntervalMs) {
    lastPublishMs = now;
    publishTelemetry();
  }
}
