#define BLYNK_PRINT Serial
#define BLYNK_TEMPLATE_ID "TMPxxxxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "Bio-Telematics"
#define BLYNK_AUTH_TOKEN "Your_Auth_Token"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_ADS1X15.h>
#include <DHT.h>
#include <time.h>

char ssid[] = "YOUR_WIFI_NAME";
char pass[] = "WIFI_PASSWORD";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;
const int DAY_START_HOUR = 6;
const int DAY_END_HOUR = 19;

#define DHTPIN 4
#define DHTTYPE DHT22

BH1750 lightMeter;
Adafruit_ADS1115 ads;
DHT dht(DHTPIN, DHTTYPE);
BlynkTimer timer;

const int DRY_VALUE = 11930;
const int WET_VALUE = 6688;
const int MAX_LUX = 2000;

const float TEMP_LOW = 15.0,  TEMP_HIGH = 35.0;
const float HUM_LOW  = 30.0,  HUM_HIGH  = 80.0;
const int   SOIL_LOW = 30,    SOIL_HIGH = 70;

const long SEND_INTERVAL_MS = 5000;

float bioBaseline = 0;
bool baselineSet = false;
const int BASELINE_SAMPLES = 12;
int baselineCount = 0;
float baselineSum = 0;

const float MILD_STRESS_THRESHOLD   = 0.015;
const float SEVERE_STRESS_THRESHOLD = 0.035;

float filteredBioVolts = 0;
bool filterInitialized = false;
const float FILTER_ALPHA = 0.20;
const float BASELINE_ALPHA = 0.002;

int scoreInRange(float value, float low, float high) {
  if (value >= low && value <= high) return 100;
  float dist = (value < low) ? (low - value) : (value - high);
  float range = high - low;
  int score = 100 - (int)((dist / range) * 150);
  return constrain(score, 0, 100);
}

bool isDaytime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return true;
  }
  int hr = timeinfo.tm_hour;
  return (hr >= DAY_START_HOUR && hr < DAY_END_HOUR);
}

int lightHealthScore(float luxVal, bool isDay) {
  if (!isDay) return 100;
  const float LUX_FLOOR = 150;
  const float EXTREME_HIGH = 100000;
  if (luxVal < LUX_FLOOR) {
    float score = 100.0 - ((LUX_FLOOR - luxVal) / LUX_FLOOR) * 100.0;
    return constrain((int)score, 0, 100);
  } else if (luxVal > EXTREME_HIGH) {
    float excess = luxVal - EXTREME_HIGH;
    float score = 100.0 - (excess / EXTREME_HIGH) * 100.0;
    return constrain((int)score, 0, 100);
  }
  return 100;
}

void sendSensorData() {
  float lux = lightMeter.readLightLevel();

  int16_t soilRaw = ads.readADC_SingleEnded(1);
  int soilPercent = map(soilRaw, DRY_VALUE, WET_VALUE, 0, 100);
  soilPercent = constrain(soilPercent, 0, 100);

  int16_t bioRaw = ads.readADC_SingleEnded(0);
  float bioVolts = ads.computeVolts(bioRaw);

  if (!filterInitialized) {
    filteredBioVolts = bioVolts;
    filterInitialized = true;
  } else {
    filteredBioVolts = (1.0 - FILTER_ALPHA) * filteredBioVolts + FILTER_ALPHA * bioVolts;
  }
  bioVolts = filteredBioVolts;

  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  bool dhtOk = !isnan(temp) && !isnan(hum);

  int lightPercent = map((int)lux, 0, MAX_LUX, 0, 100);
  lightPercent = constrain(lightPercent, 0, 100);

  if (!baselineSet) {
    baselineSum += bioVolts;
    baselineCount++;
    Serial.print("Establishing baseline... (");
    Serial.print(baselineCount); Serial.print("/"); Serial.print(BASELINE_SAMPLES);
    Serial.println(")");
    if (baselineCount >= BASELINE_SAMPLES) {
      bioBaseline = baselineSum / BASELINE_SAMPLES;
      baselineSet = true;
      Serial.print("Baseline set: "); Serial.println(bioBaseline, 4);
    }
    return;
  }

  bioBaseline = (1.0 - BASELINE_ALPHA) * bioBaseline + BASELINE_ALPHA * bioVolts;
  float deviation = abs(bioVolts - bioBaseline);

  const char* bioStatus;
  if (deviation >= SEVERE_STRESS_THRESHOLD) {
    bioStatus = "SEVERE STRESS";
  } else if (deviation >= MILD_STRESS_THRESHOLD) {
    bioStatus = "Mild Stimulus";
  } else {
    bioStatus = "Stable";
  }

  bool daytime = isDaytime();
  int tempScore  = dhtOk ? scoreInRange(temp, TEMP_LOW, TEMP_HIGH) : 50;
  int humScore   = dhtOk ? scoreInRange(hum, HUM_LOW, HUM_HIGH) : 50;
  int soilScore  = scoreInRange(soilPercent, SOIL_LOW, SOIL_HIGH);
  int lightScore = lightHealthScore(lux, daytime);
  int overallScore = (tempScore + humScore + soilScore + lightScore) / 4;

  const char* healthStatus;
  const char* recommendation;
  int lowestScore = min(min(tempScore, humScore), min(soilScore, lightScore));
  int lowestCriticalScore = lowestScore;

  if (lowestCriticalScore <= 10) {
    healthStatus = "Critical";
  } else if (lowestCriticalScore <= 30) {
    healthStatus = (overallScore < 50) ? "Critical" : "Stressed";
  } else if (overallScore >= 85) {
    healthStatus = "Excellent";
  } else if (overallScore >= 70) {
    healthStatus = "Healthy";
  } else if (overallScore >= 50) {
    healthStatus = "Needs Attention";
  } else if (overallScore >= 30) {
    healthStatus = "Stressed";
  } else {
    healthStatus = "Critical";
  }

  if (strcmp(healthStatus, "Excellent") == 0) {
    recommendation = "Conditions are optimal. No action needed.";
  } else if (strcmp(healthStatus, "Healthy") == 0) {
    recommendation = "Conditions are good. Keep monitoring.";
  } else {
    if (lowestScore == soilScore) recommendation = "Soil moisture is out of range - consider watering or checking drainage.";
    else if (lowestScore == lightScore) recommendation = "Light levels are out of range - consider repositioning the plant.";
    else if (lowestScore == tempScore) recommendation = "Temperature is out of the healthy range for this plant.";
    else recommendation = "Humidity is out of the healthy range.";
  }

  if (strcmp(bioStatus, "SEVERE STRESS") == 0) {
    recommendation = "Strong bio-electrical stress response detected. Inspect plant for physical damage or acute stress immediately.";
  }

  static bool severeAlertSent = false;
  static bool criticalAlertSent = false;

  if (strcmp(bioStatus, "SEVERE STRESS") == 0 && !severeAlertSent) {
    Blynk.logEvent("severe_stress", "Severe bio-electrical stress detected on plant!");
    severeAlertSent = true;
  } else if (strcmp(bioStatus, "SEVERE STRESS") != 0) {
    severeAlertSent = false;
  }

  if (strcmp(healthStatus, "Critical") == 0 && !criticalAlertSent) {
    char eventMsg[64];
    snprintf(eventMsg, sizeof(eventMsg), "Plant health score is critical: %d/100", overallScore);
    Blynk.logEvent("critical_health", eventMsg);
    criticalAlertSent = true;
  } else if (strcmp(healthStatus, "Critical") != 0) {
    criticalAlertSent = false;
  }

  Serial.println("========================================");
  Serial.print("Temp: "); Serial.print(dhtOk ? String(temp,1) : "ERR"); Serial.println(" C");
  Serial.print("Hum: "); Serial.print(dhtOk ? String(hum,1) : "ERR"); Serial.println(" %");
  Serial.print("Light: "); Serial.print(lux); Serial.print(" lx ("); Serial.print(lightPercent); Serial.println("%)");
  Serial.print("Soil: "); Serial.print(soilPercent); Serial.println(" %");
  Serial.print("Bio-Potential: "); Serial.print(bioVolts, 4); Serial.print(" V | Deviation: ");
  Serial.print(deviation, 4); Serial.print(" | Status: "); Serial.println(bioStatus);
  Serial.print("Health Score: "); Serial.print(overallScore); Serial.print("/100 - "); Serial.println(healthStatus);
  Serial.print("Recommendation: "); Serial.println(recommendation);
  Serial.println("========================================\n");

  if (WiFi.status() == WL_CONNECTED) {
    if (dhtOk) {
      Blynk.virtualWrite(V0, temp);
      Blynk.virtualWrite(V1, hum);
    }
    Blynk.virtualWrite(V2, lux);
    Blynk.virtualWrite(V3, soilPercent);
    Blynk.virtualWrite(V4, bioVolts);
    Blynk.virtualWrite(V5, overallScore);
    Blynk.virtualWrite(V6, healthStatus);
    Blynk.virtualWrite(V7, recommendation);
    Blynk.virtualWrite(V8, bioStatus);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(21, 22);
  Serial.println("=== Bio-Telematics System Starting ===");

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750: OK");
  } else {
    Serial.println("BH1750: NOT FOUND");
  }

  if (ads.begin()) {
    Serial.println("ADS1115: OK");
  } else {
    Serial.println("ADS1115: NOT FOUND");
  }
  ads.setGain(GAIN_ONE);

  dht.begin();
  Serial.println("DHT22: initialized");

  WiFi.begin(ssid, pass);
  Serial.print("Connecting WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected");
    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect();
  } else {
    Serial.println("\nWiFi Failed - continuing offline");
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println(&timeinfo, "Time synced: %A, %B %d %Y %H:%M:%S");
  } else {
    Serial.println("Time sync failed - will retry automatically, defaulting to daytime assumption until then");
  }

  timer.setInterval(SEND_INTERVAL_MS, sendSensorData);

  Serial.println("Settling bio-potential baseline for about 1 minute, please don't touch the plant...");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
  }
  timer.run();
  delay(10);
}
