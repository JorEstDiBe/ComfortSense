# ComfortSense-IoT 🌡️💡🔊

**Universidad de la Sabana — Internet de las Cosas (2026-1)**

**Equipo:** Jorge Esteban Díaz Bernal · Carlos Augusto Sánchez Lombana · Laura Camila Rodríguez León · Andrea Paola Urdaneta Rosales

---

## 1. Visión del Proyecto

En muchos espacios de estudio y trabajo, las condiciones ambientales no son adecuadas, lo que afecta la concentración, la productividad y el bienestar de las personas. Factores como la temperatura, la iluminación o el ruido suelen pasar desapercibidos, aunque tienen un impacto directo en el rendimiento diario.

Este proyecto propone el desarrollo de un sistema IoT de bajo costo basado en un **ESP32**, capaz de monitorear variables ambientales en tiempo real y calcular un **índice de confort**. La información se envía a un dashboard accesible, permitiendo a los usuarios tomar decisiones informadas para mejorar su entorno. Está dirigido a estudiantes, oficinas y hogares que buscan optimizar sus condiciones de estudio o trabajo de forma simple y eficiente.

---

## 2. Descripción General del Sistema

El sistema captura variables ambientales mediante sensores conectados a un microcontrolador ESP32: temperatura, humedad, presión, nivel de iluminación y ruido ambiental.

El ESP32 procesa los datos, calcula un índice de confort ponderado y los envía por WiFi usando el protocolo **MQTT over TLS** hacia el broker público de HiveMQ. Un dashboard web permite visualizar las condiciones del ambiente en tiempo real y recibir alertas cuando los niveles no son adecuados.

Adicionalmente, el dispositivo expone un **servidor HTTP local** con un endpoint de healthcheck para verificar el estado del sistema sin necesidad de acceder al broker MQTT.

**Flujo general:**

```
Sensores (BME280 + BH1750 + Micrófono)
    → ESP32 (procesamiento + scoring)
        → WiFi / MQTT TLS (broker.hivemq.com:8883)
            → Dashboard web
        → HTTP local /health (red local)
```

---

## 3. Arquitectura Técnica

### Hardware

| Componente | Descripción |
|---|---|
| ESP32 | Microcontrolador principal con WiFi integrado |
| BME280 | Sensor de temperatura, humedad y presión (I2C, 0x76 o 0x77) |
| BH1750 | Sensor de luz ambiental en lux (I2C) |
| Módulo de micrófono | Sensor de ruido analógico (AO → GPIO34, DO → GPIO27) |

### Pines I2C

| Señal | GPIO |
|---|---|
| SDA | 21 |
| SCL | 22 |

### Sensores de ruido

| Señal | GPIO |
|---|---|
| Analógico (AO) | 34 |
| Digital (DO) | 27 |

---

## 4. Temas MQTT

El broker utilizado es **broker.hivemq.com** en el puerto **8883 (TLS)**. El dispositivo se identifica con el `device_id` `comfortsense-esp32-001`.

| Tópico | Rol ESP32 | Descripción |
|---|---|---|
| `comfortsense/esp32-001/telemetry` | **Publica** | Datos de sensores, scores individuales e índice de confort. Se publica cada 5 segundos (configurable). |
| `comfortsense/esp32-001/status` | **Publica** | Estado del dispositivo: online, offline, pong, interval_updated. Publicado con retain=true. |
| `comfortsense/esp32-001/cmd` | **Suscribe** | Comandos entrantes para controlar el dispositivo remotamente. |

### Comandos soportados (tópico `cmd`)

| Comando | Descripción | Respuesta |
|---|---|---|
| `ping` | Solicita estado actual del dispositivo | Publica `pong` en `status` |
| `interval:<ms>` | Cambia el intervalo de publicación (1000–60000 ms). Ejemplo: `interval:10000` | Publica `interval_updated` en `status` |

---

## 5. Endpoints API (HTTP local)

El ESP32 levanta un servidor HTTP en el **puerto 80** accesible desde la red local.

### `GET /`

Confirmación de vida del dispositivo.

**Respuesta:**
```
200 OK — text/plain
ComfortSense-IoT ESP32 activo. Usa /health para verificar estado.
```

---

### `GET /health`

Healthcheck completo del dispositivo. Devuelve estado de conectividad y la última telemetría publicada.

**Método:** `GET`  
**URL:** `http://<IP_DEL_ESP32>/health`  
**Respuesta:** `200 OK — application/json`

**Payload de respuesta (ejemplo):**
```json
{
  "status": "ok",
  "device_id": "comfortsense-esp32-001",
  "timestamp": "2026-05-27T10:30:00-05:00",
  "uptime_ms": 125400,
  "wifi_connected": true,
  "mqtt_connected": true,
  "ip": "192.168.1.105",
  "rssi": -62,
  "free_heap": 198432,
  "flash_size": 4194304,
  "publish_interval_ms": 5000,
  "last_telemetry": { ... }
}
```

---

## 6. Diagrama de Secuencia

```
ESP32                   Broker MQTT (HiveMQ TLS)        Dashboard / Cliente
  |                              |                              |
  |--- Conecta WiFi              |                              |
  |--- Sincroniza NTP            |                              |
  |--- connect() con LWT ------->|                              |
  |<-- CONNACK ------------------|                              |
  |--- publish status "online" ->|                              |
  |--- subscribe cmd ----------->|                              |
  |                              |                              |
  |  (cada 5 s)                  |                              |
  |--- Lee BME280                |                              |
  |--- Lee BH1750                |                              |
  |--- Lee micrófono             |                              |
  |--- Calcula scores            |                              |
  |--- Calcula índice de confort |                              |
  |--- publish telemetry ------->|------ forward telemetry ---->|
  |                              |                              |
  |  (si llega comando)          |                              |
  |<-- publish cmd "ping" -------|<----- publish cmd "ping" ----|
  |--- publish status "pong" --->|------ forward status ------->|
  |                              |                              |
  |  (al desconectarse / LWT)    |                              |
  |                              |--- publish status "offline" ->|
```

---

## 7. Payload MQTT — Telemetría

**Tópico:** `comfortsense/esp32-001/telemetry`

```json
{
  "device_id": "comfortsense-esp32-001",
  "timestamp": "2026-05-27T10:30:00-05:00",
  "uptime_ms": 125400,
  "sensors": {
    "bme280_available": true,
    "temperature_c": 23.45,
    "humidity_percent": 55.20,
    "pressure_hpa": 1013.25,
    "bh1750_available": true,
    "light_lux": 420.50,
    "noise_peak_to_peak_adc": 312,
    "noise_voltage_pp": 0.251,
    "noise_relative_percent": 7.62,
    "noise_digital_do": 0
  },
  "scores": {
    "temperature": 95.00,
    "humidity": 100.00,
    "light": 80.00,
    "noise": 72.50
  },
  "comfort": {
    "index": 83.13,
    "status": "Confortable"
  },
  "connectivity": {
    "wifi_rssi": -62,
    "ip": "192.168.1.105",
    "mqtt_connected": true
  }
}
```

**Estados posibles de `comfort.status`:**

| Rango del índice | Estado |
|---|---|
| 70 – 100 | Confortable |
| 40 – 69 | Aceptable |
| 0 – 39 | Poco confortable |

---

## 8. Payload MQTT — Status

**Tópico:** `comfortsense/esp32-001/status` (retain = true)

```json
{
  "device_id": "comfortsense-esp32-001",
  "state": "online",
  "timestamp": "2026-05-27T10:30:00-05:00",
  "uptime_ms": 125400,
  "wifi_connected": true,
  "mqtt_connected": true,
  "ip": "192.168.1.105",
  "rssi": -62,
  "free_heap": 198432,
  "flash_size": 4194304
}
```

**Valores posibles de `state`:** `online`, `offline`, `pong`, `interval_updated`

---

## 9. Índice de Confort — Lógica de Cálculo

Cada variable ambiental recibe un puntaje de 0 a 100 basado en rangos ideales y absolutos. El índice final es una suma ponderada:

| Variable | Peso | Rango ideal | Rango absoluto |
|---|---|---|---|
| Temperatura | 25% | 20–26 °C | 16–32 °C |
| Humedad | 10% | 40–60 % | 25–90 % |
| Iluminación | 30% | 300–750 lux | 50–1200 lux |
| Ruido | 35% | < 40 ADC pp | > 1000 ADC pp = 0 |

**Fórmula:**
```
Índice = 0.25 × score_temp + 0.10 × score_hum + 0.30 × score_luz + 0.35 × score_ruido
```

---

## 10. Librerías Utilizadas

| Librería | Versión recomendada | Uso |
|---|---|---|
| `WiFi.h` | (incluida en ESP32 core) | Conexión a red WiFi |
| `WiFiClientSecure.h` | (incluida en ESP32 core) | Cliente TLS para MQTT seguro |
| `PubSubClient` | 2.8+ | Cliente MQTT |
| `WebServer.h` | (incluida en ESP32 core) | Servidor HTTP local |
| `Wire.h` | (incluida en Arduino core) | Comunicación I2C |
| `BH1750` | 1.3+ | Sensor de luz BH1750 |
| `Adafruit_Sensor` | 1.1+ | Capa de abstracción de sensores Adafruit |
| `Adafruit_BME280` | 2.2+ | Sensor BME280 (temperatura, humedad, presión) |
| `time.h` | (incluida en ESP32 core) | Sincronización NTP y timestamps |

---

## 11. Uso de Memoria

> ⚠️ **Pendiente:** Este dato se obtiene desde el IDE de Arduino al momento de verificar o cargar el sketch. Actualizar con los valores reales del equipo.

```
Sketch uses XXXXXX bytes (XX%) of program storage space. Maximum is 1,310,720 bytes.
Global variables use XXXXX bytes (XX%) of dynamic memory, leaving XXXXX bytes for local variables. Maximum is 327,680 bytes.
```

**Notas sobre memoria en tiempo de ejecución:**
- El buffer MQTT fue ampliado a **1024 bytes** para soportar el payload de telemetría.
- El heap libre puede consultarse en tiempo real desde el endpoint `/health` (`free_heap`).
- El tamaño del flash se reporta también en `/health` (`flash_size`).

---

## 12. Restricciones y Limitaciones

- **Memoria limitada:** El ESP32 no es adecuado para procesamiento pesado ni almacenamiento local extenso. El payload JSON de telemetría es grande; si se agregan más campos puede superar el buffer MQTT.
- **Dependencia de red WiFi:** La transmisión por MQTT requiere conexión estable. Sin WiFi no hay publicación de datos.
- **Broker público sin autenticación:** Se usa `broker.hivemq.com` sin usuario/contraseña. Los tópicos son accesibles por cualquier cliente que conozca el nombre. No apto para producción con datos sensibles.
- **Precisión del sensor de ruido:** La lectura analógica pico a pico es una aproximación relativa del nivel de ruido, no una medición calibrada en decibelios.
- **Precisión del BME280:** Puede requerir calibración en entornos con variaciones bruscas. El sensor puede necesitar tiempo de estabilización tras el encendido.
- **Sin almacenamiento histórico local:** Los datos no se guardan en la tarjeta de memoria ni en flash. Si el broker no los persiste, se pierden.
- **Escalabilidad limitada:** Agregar sensores adicionales o lógica compleja puede aumentar la carga del microcontrolador y el tamaño del payload.
- **Consumo energético:** El sistema opera por USB. Si se requiere alimentación por batería, es necesario optimizar los ciclos de sleep.

---

## 13. Posibilidades de Mejora

- **Autenticación MQTT:** Migrar a HiveMQ Cloud con usuario, contraseña y certificado propio para asegurar los datos.
- **Sensor de calidad del aire (MQ-135):** Agregar CO₂ y gases como variable adicional del índice de confort.
- **Almacenamiento histórico:** Integrar InfluxDB o un servicio de time-series para guardar mediciones y visualizar tendencias.
- **Alertas push:** Notificaciones por Telegram, correo o dashboard cuando el índice de confort baja de un umbral.
- **Modo deep sleep:** Reducir consumo energético entre publicaciones para permitir operación con batería.
- **OTA (Over The Air):** Actualizaciones de firmware por WiFi sin necesidad de conexión física.
- **Calibración del sensor de ruido:** Mapear el ADC pico a pico a decibelios mediante calibración con medidor certificado.
- **Dashboard avanzado:** Agregar gráficas históricas, tendencias y recomendaciones automáticas según el índice de confort.
- **Multi-dispositivo:** Escalar a varios ESP32 publicando en tópicos diferenciados por ID para monitorear múltiples espacios.
- **Lógica de reconexión robusta:** Implementar backoff exponencial en reconexiones WiFi y MQTT.

---

## 14. Presupuesto Estimado

| Componente | Cantidad | Costo aproximado (COP) |
|---|---|---|
| ESP32 | 1 | 35,000 – 45,000 |
| BME280 | 1 | 15,000 – 35,000 |
| BH1750 | 1 | 10,000 – 20,000 |
| Módulo micrófono (MQ-135 opcional) | 1 | 12,000 – 25,000 |
| Protoboard y cables | 1 set | 15,000 – 25,000 |
| **Total estimado** | | **75,000 – 150,000 COP** |

---

## 15. MVP y Backlog

### Funcionalidades Must-have ✅
- Lectura de temperatura y humedad (BME280)
- Lectura de nivel de iluminación (BH1750)
- Lectura de ruido ambiental (módulo micrófono)
- Procesamiento y cálculo del índice de confort en el ESP32
- Envío de datos por WiFi usando MQTT TLS
- Visualización en dashboard básico
- Healthcheck HTTP local

### Funcionalidades Nice-to-have 🔜
- Medición de calidad del aire (MQ-135)
- Generación de alertas automáticas
- Almacenamiento de datos históricos
- Visualización avanzada con gráficas y tendencias
- Recomendaciones automáticas basadas en el índice

El backlog completo está organizado en el **GitHub Project** del repositorio, con tareas clasificadas en Must-have, Nice-to-have y Spikes.

---

## 16. Cronograma

| Release | Semanas | Objetivo |
|---|---|---|
| Release 1 | 1–2 | Fundamentos y viabilidad (Spike arquitectónico) |
| Release 2 | 3–4 | Implementación del MVP funcional |
| Release 3 | 5–6 | Mejoras y funcionalidades adicionales |
| Release 4 | 7–8 | Pulido y entrega final |

---

## 17. Configuración y Uso

1. Clonar el repositorio e instalar las librerías listadas en la sección 10.
2. Editar en el sketch las credenciales WiFi (`WIFI_SSID`, `WIFI_PASSWORD`).
3. Verificar y cargar el sketch en el ESP32 desde Arduino IDE.
4. Abrir el Monitor Serial a **115200 baud** para ver logs de conexión y lecturas.
5. Consultar el healthcheck en `http://<IP_DEL_ESP32>/health` desde la misma red.
6. Suscribirse al tópico `comfortsense/esp32-001/telemetry` en cualquier cliente MQTT para recibir datos en tiempo real.

---

*ComfortSense-IoT — Universidad de la Sabana, 2026*
