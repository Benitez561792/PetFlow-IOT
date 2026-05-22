
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <ArduinoJson.h>
#include <time.h>

// ── WiFi ─────────────────────────────────────────────────
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ── MQTT ─────────────────────────────────────────────────
const char* MQTT_BROKER  = "broker.hivemq.com";
const int   MQTT_PORT    = 1883;
const char* TOPIC_TEMP   = "petflow/temperatura";
const char* TOPIC_STATUS = "petflow/status";

char MQTT_CLIENT[30];

// ── NTP ──────────────────────────────────────────────────
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET = -10800;
const int   DST_OFFSET = 0;

// ── Pinos ─────────────────────────────────────────────────
const int DHT_PIN       = 15;
const int LED_OK_PIN    = 2;
const int LED_FEVER_PIN = 4;
const int BUZZER_PIN    = 5;

// ── Limites de temperatura pet (°C) ──────────────────────
const float TEMP_HYPO_MAX   = 37.5f;
const float TEMP_NORMAL_MAX = 39.2f;
const float TEMP_CRITICAL   = 40.5f;

// ── Temporização ─────────────────────────────────────────
const unsigned long PUBLISH_INTERVAL_MS = 3000;
const unsigned long MQTT_RETRY_MS       = 5000;
const unsigned long BLINK_INTERVAL_MS   = 150;
const unsigned long BLINK_SLOW_MS       = 500;
const unsigned long BUZZ_ON_MS          = 200;
const unsigned long BUZZ_OFF_MS         = 300;
const unsigned long BUZZ_CRIT_MS        = 600;
const unsigned long BUZZ_GAP_MS         = 400;
const unsigned long BUZZ_HYPO_GAP_MS    = 2000;
const unsigned long BUZZ_HYPO_ON_MS     = 100;

// ── Cenários simulados com temperaturas biológicas reais ─
//
// Referências biológicas usadas:
//   Hamster    : normal 37–38 °C  → 36.2 = hipotermia real
//   Coelho     : normal 38–39.5°C → 37.1 = hipotermia real
//   Gato       : normal 37.5–39.2°C
//   Cachorro   : normal 37.5–39.2°C
//   Porquinho  : normal 38–40°C
//   Ferret     : normal 37.8–40°C
//   Chinchila  : normal 36–38°C   → 39.5 = febre real
//


struct SimReading { float temp; float humidity; const char* pet; };

SimReading simReadings[] = {
  // ── HIPOTERMIA (temp < 37.5°C) ───────────────────────
  { 36.2f, 60.0f, "Hamster"   },  // Hamster: normal 37–38°C → 36.2 = hipotermia real
  { 37.1f, 62.0f, "Coelho"    },  // Coelho:  normal 38–39.5°C → 37.1 = hipotermia real

  // ── NORMAL (37.5°C – 39.2°C) ─────────────────────────
  { 37.6f, 65.0f, "Gato"      },  // Gato:    normal 37.5–39.2°C → limite inferior
  { 38.0f, 65.0f, "Cachorro"  },  // Cachorro: normal 37.5–39.2°C → faixa ideal
  { 38.5f, 66.0f, "Porquinho" },  // Porquinho: normal 38–40°C → faixa ideal
  { 39.0f, 67.0f, "Ferret"    },  // Ferret:  normal 37.8–40°C → faixa ideal

  // ── FEBRE (39.3°C – 40.5°C) ──────────────────────────
  { 39.5f, 68.0f, "Chinchila" },  // Chinchila: normal 36–38°C → 39.5 = febre real
  { 39.8f, 70.0f, "Gato"      },  // Gato:    normal 37.5–39.2°C → 39.8 = febre real
  { 40.2f, 72.0f, "Cachorro"  },  // Cachorro: normal 37.5–39.2°C → 40.2 = febre real

  // ── CRÍTICO (> 40.5°C) ───────────────────────────────
  { 40.6f, 74.0f, "Coelho"    },  // Coelho:  normal 38–39.5°C → 40.6 = crítico real
  { 41.2f, 75.0f, "Hamster"   },  // Hamster: normal 37–38°C → 41.2 = crítico real
  { 41.8f, 76.0f, "Ferret"    },  // Ferret:  normal 37.8–40°C → 41.8 = crítico real

  // ── Recuperação (descendo) ────────────────────────────
  { 40.0f, 72.0f, "Porquinho" },  // Porquinho: voltando da febre
  { 39.0f, 68.0f, "Cachorro"  },  // Cachorro: normalizando
  { 38.2f, 65.0f, "Gato"      },  // Gato:    temperatura normal novamente
};

const int SIM_COUNT = sizeof(simReadings) / sizeof(simReadings[0]);
int simIndex = 0;

// ── Objetos globais ───────────────────────────────────────
DHTesp       dht;
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastPublishTime = 0;
unsigned long lastMqttRetry   = 0;
int           readingCount    = 0;

String        currentStatus = "NORMAL";
unsigned long lastBlinkTime = 0;
unsigned long lastBuzzTime  = 0;
bool          blinkState    = false;
bool          buzzActive    = false;

// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔══════════════════════════════╗");
  Serial.println("║   PetFlow IoT — Starting...  ║");
  Serial.println("╚══════════════════════════════╝");

  snprintf(MQTT_CLIENT, 30, "petflow-%08lX", (unsigned long)millis());
  Serial.println("[MQTT] Client ID: " + String(MQTT_CLIENT));

  // Pull-up interno ativado no pino de dados do DHT22
  // (complementa o resistor externo de 10kΩ do circuito)
  pinMode(DHT_PIN, INPUT_PULLUP);

  pinMode(LED_OK_PIN,    OUTPUT);
  pinMode(LED_FEVER_PIN, OUTPUT);
  pinMode(BUZZER_PIN,    OUTPUT);
  digitalWrite(LED_OK_PIN,    LOW);
  digitalWrite(LED_FEVER_PIN, LOW);
  digitalWrite(BUZZER_PIN,    LOW);

  dht.setup(DHT_PIN, DHTesp::DHT22);

  connectWiFi();
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(15);
  mqttClient.setSocketTimeout(10);

  tryConnectMQTT();

  Serial.println("[MODO] Ciclo simulado ativo (" + String(SIM_COUNT) + " cenários)");
  Serial.println("[INFO] Temperaturas biológicas reais por espécie");
  Serial.println("[INFO] Em hardware real, leitura do DHT22 físico substitui automaticamente");
}

// ─────────────────────────────────────────────────────────
void loop() {
  if (!WiFi.isConnected()) connectWiFi();

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry >= MQTT_RETRY_MS) {
      lastMqttRetry = now;
      tryConnectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  unsigned long now = millis();
  if (now - lastPublishTime >= PUBLISH_INTERVAL_MS) {
    lastPublishTime = now;
    readAndPublish();
  }

  updateAlerts(now);
}

// ─────────────────────────────────────────────────────────
void tryConnectMQTT() {
  Serial.print("[MQTT] Tentando conectar a " + String(MQTT_BROKER) + "...");
  if (mqttClient.connect(MQTT_CLIENT)) {
    Serial.println(" Conectado!");
    mqttClient.publish(TOPIC_STATUS,
      "{\"online\":true,\"msg\":\"PetFlow IoT online\"}");
  } else {
    Serial.println(" Falhou rc=" + String(mqttClient.state()) + " — tentará em 5s");
  }
}

// ─────────────────────────────────────────────────────────
void readAndPublish() {
  float temperature, humidity;
  bool  usedSensor = false;

  // Tenta leitura real do DHT22
  // No Wokwi: sensor tem valor fixo configurado no diagram.json
  // Em hardware real: reflete a temperatura ambiente do pet
  TempAndHumidity reading = dht.getTempAndHumidity();

  if (dht.getStatus() == DHTesp::ERROR_NONE &&
      !isnan(reading.temperature) && !isnan(reading.humidity)) {
    temperature = reading.temperature;
    humidity    = reading.humidity;
    usedSensor  = true;
    Serial.println("[DHT22] Leitura real: " + String(temperature) + "°C / " + String(humidity) + "%");
  } else {
    temperature = simReadings[simIndex].temp;
    humidity    = simReadings[simIndex].humidity;
    simIndex    = (simIndex + 1) % SIM_COUNT;
    Serial.println("[DHT22] Usando cenário simulado #" + String(simIndex)
                   + ": " + String(temperature) + "°C");
  }

  // No Wokwi o sensor retorna valor fixo (ex: 38.5°C definido no diagram.json).
  // Para demonstrar todos os estados clínicos no dashboard, avançamos o ciclo
  // simulado independentemente e sobrescrevemos a leitura fixa do Wokwi.
  // Em hardware real (protoboard), remova as 3 linhas abaixo.
  temperature = simReadings[simIndex].temp;
  humidity    = simReadings[simIndex].humidity;
  simIndex    = (simIndex + 1) % SIM_COUNT;

  // Nome do pet correspondente à leitura atual
  const char* petName = simReadings[(simIndex == 0 ? SIM_COUNT - 1 : simIndex - 1)].pet;

  readingCount++;

  String status, statusColor;
  if (temperature < TEMP_HYPO_MAX) {
    status = "HIPOTERMIA"; statusColor = "blue";
  } else if (temperature <= TEMP_NORMAL_MAX) {
    status = "NORMAL";     statusColor = "green";
  } else if (temperature <= TEMP_CRITICAL) {
    status = "FEBRE";      statusColor = "orange";
  } else {
    status = "CRITICO";    statusColor = "red";
  }

  currentStatus = status;

  struct tm timeinfo;
  char isoTime[25] = "sem_hora";
  if (getLocalTime(&timeinfo)) {
    strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  }

  Serial.printf("\n[#%04d] Pet: %s | Temp: %.1f°C | Umid: %.1f%% | Status: %s | %s\n",
                readingCount, petName, temperature, humidity, status.c_str(), isoTime);

  if (!mqttClient.connected()) {
    Serial.println("[MQTT] Sem conexão — dado registrado localmente");
    return;
  }

  StaticJsonDocument<256> doc;
  doc["device"]      = MQTT_CLIENT;
  doc["pet"]         = petName;
  doc["temperatura"] = round(temperature * 10) / 10.0;
  doc["umidade"]     = round(humidity * 10) / 10.0;
  doc["status"]      = status;
  doc["cor"]         = statusColor;
  doc["leitura"]     = readingCount;
  doc["timestamp"]   = isoTime;
  doc["fonte"]       = usedSensor ? "sensor" : "simulado";

  char payload[256];
  serializeJson(doc, payload);

  if (mqttClient.publish(TOPIC_TEMP, payload, true)) {
    Serial.println("[MQTT] Publicado → " + String(TOPIC_TEMP));
    Serial.println("[MQTT] Payload  : " + String(payload));
  } else {
    Serial.println("[MQTT] ERRO ao publicar!");
  }

  StaticJsonDocument<128> statusDoc;
  statusDoc["online"]    = true;
  statusDoc["uptime_s"]  = millis() / 1000;
  statusDoc["timestamp"] = isoTime;
  char statusPayload[128];
  serializeJson(statusDoc, statusPayload);
  mqttClient.publish(TOPIC_STATUS, statusPayload, true);
}

// ─────────────────────────────────────────────────────────
void updateAlerts(unsigned long now) {

  if (currentStatus == "NORMAL") {
    digitalWrite(LED_OK_PIN,    HIGH);
    digitalWrite(LED_FEVER_PIN, LOW);
    digitalWrite(BUZZER_PIN,    LOW);
    buzzActive = false;

  } else if (currentStatus == "HIPOTERMIA") {
    // LED vermelho piscando lento + 1 bip curto a cada 2s
    digitalWrite(LED_OK_PIN, LOW);
    if (now - lastBlinkTime >= BLINK_SLOW_MS) {
      blinkState = !blinkState;
      digitalWrite(LED_FEVER_PIN, blinkState ? HIGH : LOW);
      lastBlinkTime = now;
    }
    if (!buzzActive && (now - lastBuzzTime >= BUZZ_HYPO_GAP_MS)) {
      digitalWrite(BUZZER_PIN, HIGH);
      lastBuzzTime = now; buzzActive = true;
    } else if (buzzActive && (now - lastBuzzTime >= BUZZ_HYPO_ON_MS)) {
      digitalWrite(BUZZER_PIN, LOW);
      lastBuzzTime = now; buzzActive = false;
    }

  } else if (currentStatus == "FEBRE") {
    // LED vermelho fixo + buzzer intermitente moderado
    digitalWrite(LED_OK_PIN, LOW);
    digitalWrite(LED_FEVER_PIN, HIGH);
    if (!buzzActive && (now - lastBuzzTime >= BUZZ_OFF_MS)) {
      digitalWrite(BUZZER_PIN, HIGH);
      lastBuzzTime = now; buzzActive = true;
    } else if (buzzActive && (now - lastBuzzTime >= BUZZ_ON_MS)) {
      digitalWrite(BUZZER_PIN, LOW);
      lastBuzzTime = now; buzzActive = false;
    }

  } else if (currentStatus == "CRITICO") {
    // LED vermelho piscando rápido + buzzer longo
    if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
      blinkState = !blinkState;
      digitalWrite(LED_FEVER_PIN, blinkState ? HIGH : LOW);
      digitalWrite(LED_OK_PIN, LOW);
      lastBlinkTime = now;
    }
    if (!buzzActive && (now - lastBuzzTime >= BUZZ_GAP_MS)) {
      digitalWrite(BUZZER_PIN, HIGH);
      lastBuzzTime = now; buzzActive = true;
    } else if (buzzActive && (now - lastBuzzTime >= BUZZ_CRIT_MS)) {
      digitalWrite(BUZZER_PIN, LOW);
      lastBuzzTime = now; buzzActive = false;
    }
  }
}

// ─────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Conectando a " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (!WiFi.isConnected() && attempts < 40) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.isConnected()) {
    Serial.println("\n[WiFi] Conectado! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Falha na conexão!");
  }
}
