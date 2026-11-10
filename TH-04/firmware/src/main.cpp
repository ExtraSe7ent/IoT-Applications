#include <Arduino.h>
#include <DHT.h>
#include <esp_arduino_version.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <atomic>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mqtt_client.h>

#if __has_include("th04_secrets.h")
#include "th04_secrets.h"
#else
#include "th04_secrets.example.h"
#endif

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "TH03 requires Arduino-ESP32 3.x (timerBegin/ledcAttach APIs)."
#endif

// Farm House rev1.0.1, sheet 4/5: ESP32-DEVKITC-32E.
constexpr uint8_t LOAD_PIN = 13;    // ARRAY_LED3 -> U11 -> Q10 -> J11.
constexpr uint8_t DHT_PIN = 14;     // DATA_RH -> R28 -> J1, DHT11.
constexpr uint8_t BUTTON_PIN = 36;  // FUNC_BUT / SENSOR_VP: external pull-up required.
constexpr uint32_t MAX_RUN_MS = 15000;
constexpr uint32_t SENSOR_PERIOD_MS = 2000;
constexpr uint32_t SENSOR_STALE_MS = 3000;
constexpr uint32_t TIMER_PERIOD_US = 2000000;
constexpr uint32_t DEBOUNCE_MS = 25;
constexpr uint32_t PWM_HZ = 1000;
constexpr uint8_t PWM_BITS = 8;

// Assumption: HIGH at U11 VIN turns Q10 ON. Verify U11 polarity with load
// disconnected before applying VDC. Software cannot guarantee reset-time OFF.
enum class State : uint8_t { STOPPED, RUNNING, FAULT };
State state = State::STOPPED;
DHT dht(DHT_PIN, DHT11);
hw_timer_t *hardwareTimer = nullptr;
volatile uint32_t buttonEdges = 0;
volatile uint32_t timerTicks = 0;

bool pwmReady = false, hardwareReady = false, sensorValid = false;
bool restartLocked = false, debounceEnabled = true;
bool requestStart = false, requestStop = false, requestReset = false;
const char *pendingFault = nullptr;
uint32_t startedAt = 0, lastSensorAt = 0, lastValidAt = 0;
uint8_t dutyPercent = 50, outputDuty = 0;
float temperature = NAN, humidity = NAN;

constexpr char TELEMETRY_TOPIC[] = "23110149/node01/telemetry";
constexpr UBaseType_t TELEMETRY_QUEUE_DEPTH = 10;

struct TelemetrySample {
  uint32_t timestampMs;
  float temperature;
  float humidity;
  bool dhtValid;
};

QueueHandle_t telemetryQueue = nullptr;
uint32_t queueDrops = 0;
std::atomic<bool> mqttConnected{false};

// ISR bodies only increment counters. No Serial, delay, DHT or PWM here.
void ARDUINO_ISR_ATTR onButton() { ++buttonEdges; }
void ARDUINO_ISR_ATTR onTimer() { ++timerTicks; }

const char *stateName() {
  switch (state) {
    case State::RUNNING: return "CHAY";
    case State::FAULT: return "LOI";
    default: return "DUNG";
  }
}

void switchOff() {
  if (pwmReady && !ledcWrite(LOAD_PIN, 0)) {
    ledcDetach(LOAD_PIN);
    pwmReady = false;
    hardwareReady = false;
    restartLocked = true;
  }
  if (!pwmReady) {
    digitalWrite(LOAD_PIN, LOW);
    pinMode(LOAD_PIN, OUTPUT);
  }
  outputDuty = 0;
}

void reportState(const char *reason) {
  Serial.printf("STATE,%lu,%s,duty_raw=%u,locked=%u,reason=%s\n",
                millis(), stateName(), outputDuty, restartLocked, reason);
}

void stopLoad(const char *reason, bool lockRestart) {
  switchOff();
  if (lockRestart) restartLocked = true;
  state = State::STOPPED;
  reportState(reason);
}

bool sensorUsable(uint32_t now) {
  return sensorValid && uint32_t(now - lastValidAt) <= SENSOR_STALE_MS;
}

// Highest priority: fault -> timeout -> OFF -> RESET -> ON.
// Requests are consumed once; ON while running never extends the deadline.
void updateState() {
  const uint32_t now = millis();
  const bool start = requestStart, stop = requestStop, reset = requestReset;
  const char *fault = pendingFault;
  requestStart = requestStop = requestReset = false;
  pendingFault = nullptr;
  if (!fault && state == State::RUNNING && !sensorUsable(now)) {
    fault = "sensor_invalid_or_stale";
  }
  if (fault) {
    switchOff();                 // Cut power command before printing anything.
    restartLocked = true;
    state = State::FAULT;
    reportState(fault);
    stopLoad("fault_to_stop", true);
    return;
  }
  if (state == State::RUNNING && uint32_t(now - startedAt) >= MAX_RUN_MS) {
    stopLoad("timeout", true);
    return;
  }
  if (stop) {
    stopLoad("off_command", false);
    return;
  }
  if (reset) {
    if (state == State::STOPPED && hardwareReady && sensorUsable(now)) {
      restartLocked = false;
      reportState("reset_ok_send_new_ON");
    } else {
      reportState("reset_rejected");
    }
    return;                      // RESET + ON in one batch cannot start a load.
  }
  if (start && state == State::STOPPED) {
    if (restartLocked || !hardwareReady || !sensorUsable(now)) {
      reportState("start_rejected");
      return;
    }
    const uint8_t raw = (uint16_t(dutyPercent) * 255 + 50) / 100;
    if (!ledcWrite(LOAD_PIN, raw)) {
      pendingFault = "pwm_write_failed";
      switchOff();
      return;
    }
    outputDuty = raw;
    startedAt = millis();
    state = State::RUNNING;
    reportState("on_command");
  }
}

void sampleSensor() {
  const uint32_t now = millis();
  if (uint32_t(now - lastSensorAt) < SENSOR_PERIOD_MS) return;
  lastSensorAt = now;
  // The DHT library has a short blocking transaction; this is not hard real-time.
  humidity = dht.readHumidity();
  temperature = dht.readTemperature();
  sensorValid = isfinite(temperature) && isfinite(humidity) &&
                humidity >= 0 && humidity <= 100;
  if (sensorValid) lastValidAt = millis();
  else if (state == State::RUNNING) pendingFault = "dht_read_failed";
  updateState();                 // Act on a failed read before logging values.
  if (sensorValid) {
    Serial.printf("SENSOR,%lu,valid=1,T=%.1f,H=%.1f\n",
                  millis(), temperature, humidity);
  } else {
    Serial.printf("SENSOR,%lu,valid=0,T=null,H=null\n", millis());
  }

  const TelemetrySample sample{now, temperature, humidity, sensorValid};
  if (xQueueSend(telemetryQueue, &sample, 0) != pdPASS) {
    ++queueDrops;
    Serial.printf("QUEUE,%lu,full,drop_newest,total_dropped=%lu\n",
                  millis(), (unsigned long)queueDrops);
  }
}

uint32_t buttonBase = 0, previousEdges = 0, acceptedPresses = 0;
uint32_t buttonChangedAt = 0;
bool rawButton = HIGH, stableButton = HIGH, edgePending = false;

void serviceButton() {
  const uint32_t now = millis();
  const uint32_t edges = buttonEdges; // Aligned 32-bit snapshot, single ISR writer.
  const uint32_t delta = edges - previousEdges;
  previousEdges = edges;
  const bool level = digitalRead(BUTTON_PIN);
  if (level != rawButton) {
    rawButton = level;
    buttonChangedAt = now;
  }
  if (delta) edgePending = true;
  bool changed = delta != 0;
  if (!debounceEnabled) {
    acceptedPresses += delta;
    stableButton = rawButton;
    edgePending = false;
  } else if (uint32_t(now - buttonChangedAt) >= DEBOUNCE_MS &&
             stableButton != rawButton) {
    stableButton = rawButton;
    if (stableButton == LOW && edgePending) {
      ++acceptedPresses;
      changed = true;
    }
    edgePending = false;
  }
  if (changed) {
    Serial.printf("BUTTON,%lu,raw=%lu,accepted=%lu,debounce=%u\n",
                  now, (unsigned long)(edges - buttonBase),
                  (unsigned long)acceptedPresses, debounceEnabled);
  }
}

uint32_t previousTicks = 0, previousServiceUs = 0;
bool timerSeen = false;
void serviceTimer() {
  const uint32_t ticks = timerTicks;
  const uint32_t delta = ticks - previousTicks;
  if (!delta) return;
  const uint32_t nowUs = micros();
  // This measures delivery to the safety task, including its latency.
  Serial.printf("TIMER,%lu,tick=%lu,delta_ticks=%lu,service_dt_us=%lu\n",
                millis(), (unsigned long)ticks, (unsigned long)delta,
                (unsigned long)(timerSeen ? nowUs - previousServiceUs : 0));
  previousServiceUs = nowUs;
  previousTicks = ticks;
  timerSeen = true;
}

void status() {
  reportState("status");
  Serial.printf("CONFIG,load_gpio=13,dht_gpio=14,button_gpio=36,"
                "max_run_ms=%lu,pwm_hz=%lu,duty_pct=%u,timer_us=%lu\n",
                (unsigned long)MAX_RUN_MS, (unsigned long)PWM_HZ,
                dutyPercent, (unsigned long)TIMER_PERIOD_US);
  Serial.printf("COUNTS,raw=%lu,accepted=%lu,debounce=%u\n",
                (unsigned long)(buttonEdges - buttonBase),
                (unsigned long)acceptedPresses, debounceEnabled);
}

void parseCommand(const char *line) {
  if (!strcmp(line, "ON")) requestStart = true;
  else if (!strcmp(line, "OFF")) requestStop = true;
  else if (!strcmp(line, "RESET")) requestReset = true;
  else if (!strcmp(line, "STATUS")) status();
  else if (!strcmp(line, "DUTY 25") || !strcmp(line, "DUTY 50") ||
           !strcmp(line, "DUTY 90")) {
    if (state == State::RUNNING || requestStart) {
      pendingFault = "change_duty_requires_stop";
    } else {
      dutyPercent = uint8_t(atoi(line + 5));
      Serial.printf("DUTY,%u\n", dutyPercent);
    }
  } else if (!strcmp(line, "DEBOUNCE ON") || !strcmp(line, "DEBOUNCE OFF")) {
    if (state != State::STOPPED || requestStart) {
      pendingFault = "debounce_change_requires_stop";
    } else {
      debounceEnabled = !strcmp(line, "DEBOUNCE ON");
      Serial.printf("DEBOUNCE,%u,use_CLEAR_before_10_presses\n", debounceEnabled);
    }
  } else if (!strcmp(line, "CLEAR")) {
    if (state != State::STOPPED || digitalRead(BUTTON_PIN) != HIGH || requestStart) {
      pendingFault = "clear_requires_stop_and_released_button";
    } else {
      buttonBase = previousEdges = buttonEdges;
      acceptedPresses = 0;
      edgePending = false;
      rawButton = stableButton = HIGH;
      buttonChangedAt = millis();
      Serial.println("COUNTS_CLEARED");
    }
  } else {
    pendingFault = "bad_command";
  }
}

char commandBuffer[48];
size_t commandLength = 0;
bool discardLine = false;
void readCommands() {
  // Bounded work: a continuous stream cannot starve updateState().
  for (uint8_t budget = 0; budget < 64 && Serial.available(); ++budget) {
    const char c = char(Serial.read());
    if (c == '\n') {
      if (!discardLine) {
        if (commandLength && commandBuffer[commandLength - 1] == '\r') --commandLength;
        commandBuffer[commandLength] = '\0';
        if (commandLength) parseCommand(commandBuffer);
      }
      commandLength = 0;
      discardLine = false;
    } else if (!discardLine) {
      if (commandLength >= sizeof(commandBuffer) - 1 || c == '\0') {
        pendingFault = "command_overflow_or_nul";
        discardLine = true;
        commandLength = 0;
      } else {
        commandBuffer[commandLength++] = c;
      }
    }
  }
}

void safetyTask(void *) {
  for (;;) {
    updateState();
    readCommands();
    updateState();
    serviceButton();
    sampleSensor();
    updateState();
    serviceTimer();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void mqttEvent(void *, esp_event_base_t, int32_t eventId, void *) {
  if (eventId == MQTT_EVENT_CONNECTED || eventId == MQTT_EVENT_DISCONNECTED) {
    const bool connected = eventId == MQTT_EVENT_CONNECTED;
    mqttConnected.store(connected);
    Serial.printf("MQTT,%lu,%s\n", millis(),
                  connected ? "connected" : "disconnected");
  }
}

void mqttTask(void *) {
  if (!WIFI_SSID[0] || !MQTT_PASSWORD[0]) {
    Serial.println("MQTT,fill_th04_secrets_h");
    vTaskDelete(nullptr);
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  esp_mqtt_client_config_t config{};
  config.broker.address.uri = MQTT_BROKER_URI;
  config.credentials.username = MQTT_USERNAME;
  config.credentials.authentication.password = MQTT_PASSWORD;
  config.network.reconnect_timeout_ms = 2000;
  config.network.timeout_ms = 2000;

  const auto client = esp_mqtt_client_init(&config);
  if (!client ||
      esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqttEvent, nullptr) != ESP_OK ||
      esp_mqtt_client_start(client) != ESP_OK) {
    if (client) esp_mqtt_client_destroy(client);
    Serial.println("MQTT,start_failed");
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    if (!mqttConnected.load()) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    TelemetrySample sample{};
    if (xQueueReceive(telemetryQueue, &sample, pdMS_TO_TICKS(200)) != pdPASS) continue;

    char temp[20] = "null";
    char humidityPct[20] = "null";
    if (sample.dhtValid) {
      snprintf(temp, sizeof(temp), "%.1f", sample.temperature);
      snprintf(humidityPct, sizeof(humidityPct), "%.1f", sample.humidity);
    }

    // The rain input is on the UNO. Its transport to ESP32 is unverified.
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"node01\",\"ts_ms\":%lu,\"temp_c\":%s,"
             "\"humidity_pct\":%s,\"rain_adc\":null,\"valid\":false}",
             (unsigned long)sample.timestampMs, temp, humidityPct);
    if (esp_mqtt_client_publish(client, TELEMETRY_TOPIC, payload, 0, 0, 0) < 0) {
      Serial.printf("MQTT,%lu,publish_failed,ts_ms=%lu\n", millis(),
                    (unsigned long)sample.timestampMs);
    }
  }
}

void setup() {
  // Farm House rev1.0.1: IN1 and IN2 low put both A4950 drivers in standby.
  constexpr gpio_num_t motorInputs[] = {GPIO_NUM_5, GPIO_NUM_17, GPIO_NUM_23, GPIO_NUM_22};
  for (gpio_num_t pin : motorInputs) {
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  }

  digitalWrite(LOAD_PIN, LOW);
  pinMode(LOAD_PIN, OUTPUT);
  Serial.begin(115200);
  Serial.println("MOTORS,standby");
  pwmReady = ledcAttach(LOAD_PIN, PWM_HZ, PWM_BITS);
  switchOff();
  pinMode(BUTTON_PIN, INPUT);     // GPIO36 has NO internal pull-up.
  rawButton = stableButton = digitalRead(BUTTON_PIN);
  buttonChangedAt = millis();
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButton, FALLING);
  dht.begin();
  hardwareTimer = timerBegin(1000000); // 1 MHz counter: one tick = one microsecond.
  if (hardwareTimer) {
    timerAttachInterrupt(hardwareTimer, onTimer);
    timerAlarm(hardwareTimer, TIMER_PERIOD_US, true, 0);
  }
  hardwareReady = pwmReady && hardwareTimer != nullptr;
  if (!hardwareReady) pendingFault = "pwm_or_timer_init_failed";
  lastSensorAt = millis();       // Allow DHT to settle for two seconds.
  reportState("boot");
  Serial.println("COMMANDS: ON | OFF | RESET | STATUS | DUTY 25/50/90 | DEBOUNCE ON/OFF | CLEAR");
  updateState();

  telemetryQueue = xQueueCreate(TELEMETRY_QUEUE_DEPTH, sizeof(TelemetrySample));
  if (!telemetryQueue ||
      xTaskCreate(safetyTask, "safety", 4096, nullptr, 2, nullptr) != pdPASS) {
    pendingFault = "safety_task_init_failed";
    updateState();
    return;
  }
  if (xTaskCreate(mqttTask, "mqtt_publish", 4096, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("MQTT,task_init_failed");
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
