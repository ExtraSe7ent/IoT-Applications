#include <Wire.h>
#include <DHTesp.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

constexpr bool BLOCKING = false;
constexpr int DHT_PIN = 14, ADC_PIN = 34, BUTTON_PIN = 27;
constexpr unsigned long INTERVAL_MS = 2000;

class MovingAverage {
  float values[5] = {};
  int next = 0, count = 0;
public:
  void add(float value) {
    values[next] = value;
    next = (next + 1) % 5;
    if (count < 5) ++count;
  }
  bool ready() const { return count == 5; }
  float mean() const {
    float total = 0;
    for (int i = 0; i < count; ++i) total += values[i];
    return count ? total / count : 0;
  }
};

DHTesp dht;
Adafruit_SSD1306 screen(128, 64, &Wire, -1);
MovingAverage temperatureFilter, humidityFilter;
bool screenReady = false;
unsigned long lastSample = 0, lastUi = 0, lastButtonChange = 0;
unsigned long previousSample = 0, sampleCount = 0, pressCount = 0;
bool rawButton = HIGH, stableButton = HIGH;
float temperature = NAN, humidity = NAN;
bool valid = false;
int adc = 0;
float voltage = 0;

void updateScreen() {
  if (!screenReady) return;
  screen.clearDisplay();
  screen.setCursor(0, 0);
  screen.println(BLOCKING ? "TH02: BLOCKING" : "TH02: NONBLOCKING");
  if (valid) {
    screen.print("T: "); screen.print(temperature, 1); screen.println(" C");
    screen.print("H: "); screen.print(humidity, 1); screen.println(" %");
  } else {
    screen.println("DHT: NO VALID DATA");
    screen.println("T/H: null");
  }
  screen.print("ADC: "); screen.println(adc);
  screen.print("V ideal: "); screen.println(voltage, 2);
  screen.print("Button count: "); screen.println(pressCount);
  screen.display();
}

void pollButton() {
  unsigned long now = millis();
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != rawButton) {
    rawButton = reading;
    lastButtonChange = now;
  }
  if (now - lastButtonChange >= 25 && stableButton != rawButton) {
    stableButton = rawButton;
    if (stableButton == LOW) {
      ++pressCount;
      Serial.printf("# BUTTON,%lu,count=%lu\n", now, pressCount);
      updateScreen();
    }
  }
}

void sampleSensor() {
  unsigned long started = millis();
  unsigned long dt = sampleCount ? started - previousSample : 0;
  previousSample = started;
  ++sampleCount;
  TempAndHumidity data = dht.getTempAndHumidity();
  temperature = data.temperature;
  humidity = data.humidity;
  valid = dht.getStatus() == DHTesp::ERROR_NONE &&
          !isnan(temperature) && !isnan(humidity);
  adc = analogRead(ADC_PIN);
  voltage = adc * (3.3f / 4095.0f); 
  if (valid) {
    temperatureFilter.add(temperature);
    humidityFilter.add(humidity);
  }
  Serial.printf("%lu,%lu,%lu,", started, sampleCount, dt);
  if (valid) Serial.printf("%.2f,%.2f,1,", temperature, humidity);
  else Serial.print("null,null,0,dht_read_failed");
  Serial.print(",");
  if (valid && temperatureFilter.ready()) {
    Serial.printf("%.2f,%.2f", temperatureFilter.mean(), humidityFilter.mean());
  } else Serial.print("null,null");
  Serial.printf(",%d,%.3f,%lu,%s\n", adc, voltage, pressCount,
                BLOCKING ? "blocking" : "nonblocking");
  updateScreen();
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  dht.setup(DHT_PIN, DHTesp::DHT22);
  Wire.begin(21, 22);
  bool oledFound = false;
  Serial.println("# I2C scan: SDA=21 SCL=22");
  for (int address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("# I2C_FOUND,0x%02X\n", address);
      if (address == 0x3C) oledFound = true;
    }
  }
  screenReady = oledFound && screen.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false);
  Serial.println(screenReady ? "# OLED_OK" : "# OLED_NOT_FOUND_OR_INIT_FAILED");
  if (screenReady) {
    screen.setTextSize(1);
    screen.setTextColor(SSD1306_WHITE);
    updateScreen();
  }
  Serial.println("time_ms,sample,dt_ms,temperature_c,humidity_pct,valid,error,temperature_ma5,humidity_ma5,adc_raw,voltage_ideal_v,button_count,mode");
  lastSample = millis();
}

void loop() {
  pollButton();
  if (BLOCKING) {
    delay(INTERVAL_MS); 
    sampleSensor();
  } else {
    unsigned long now = millis();
    if (now - lastSample >= INTERVAL_MS) {
      lastSample = now;
      sampleSensor();
    }
    if (now - lastUi >= 200) {
      lastUi = now;
      adc = analogRead(ADC_PIN);
      voltage = adc * (3.3f / 4095.0f);
      updateScreen();
    }
  }
}
