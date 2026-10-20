/*
  ESP32 RLCD Dashboard
  --------------------
  Desk clock and indoor climate dashboard for the Waveshare ESP32-S3-RLCD-4.2.

    - Large clock from the PCF85063 RTC, kept in time via NTP
    - Date line
    - Temperature and humidity (SHTC3) with a ~22 h chart
    - Header with WiFi signal and battery level

  All settings live in config.h; WiFi credentials go in secrets.h
  (copy secrets.example.h). See README.md for build instructions.
*/

#include <WiFi.h>
#include <Wire.h>

#include "config.h"
#include "src/battery.h"
#include "src/date_time.h"
#include "src/history.h"
#include "src/network.h"
#include "src/pcf85063.h"
#include "src/shtc3.h"
#include "src/st7305_u8g2.h"
#include "src/ui.h"

// ---------------- Hardware ----------------
static ST7305_U8g2 lcd(PIN_LCD_SCK, PIN_LCD_MOSI, PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_RST);
static Shtc3 climateSensor(Wire, I2C_ADDR_SHTC3);
static Pcf85063 rtc(Wire, I2C_ADDR_PCF85063);
static Battery battery(PIN_BATTERY_ADC);

// ---------------- State ----------------
static History history;
static DateTime currentTime;
static float temperatureC = NAN;
static float humidityPct = NAN;

static bool rtcTimeInvalid = false;  // RTC lost power; showing compile time
static bool timeSynced = false;      // NTP succeeded at least once

static uint32_t lastSensorReadMs = 0;
static uint32_t lastHistoryMs = 0;
static uint32_t lastNtpAttemptMs = 0;

// ============================================================
//                          HELPERS
// ============================================================
static constexpr uint32_t minutesToMs(uint32_t minutes) { return minutes * 60UL * 1000UL; }

static bool climateValid()
{
  return climateSensor.isPresent() && !isnan(temperatureC) && !isnan(humidityPct);
}

static void readSensors()
{
  float t, h;
  if (climateSensor.read(t, h)) {
    temperatureC = t + TEMP_OFFSET_C;
    humidityPct = h;
  }
  rtc.read(currentTime);
  battery.update();
}

// Fetches the time from NTP and stores it in the RTC.
static bool syncTimeFromNtp()
{
  if (!rtc.isPresent()) return false;

  DateTime t;
  if (!ntpFetchLocalTime(t, NTP_RESPONSE_TIMEOUT_MS)) {
    Serial.println("[ntp] No response from time server");
    return false;
  }

  rtc.write(t);
  rtcTimeInvalid = false;
  timeSynced = true;
  Serial.printf("[ntp] RTC set to %04u-%02u-%02u %02u:%02u:%02u\n",
                t.year, t.month, t.day, t.hour, t.minute, t.second);
  return true;
}

static void initRtc()
{
  if (!rtc.begin()) {
    Serial.println("[rtc] PCF85063 not detected");
    return;
  }
  if (rtc.lostPower()) {
    // Better than nothing until NTP succeeds
    rtcTimeInvalid = true;
    rtc.write(compileDateTime());
    Serial.println("[rtc] Power was lost, using compile time");
  }
}

static void drawDashboard()
{
  DashboardState s;

  s.wifiConnected = wifiConnected();
  if (s.wifiConnected) {
    s.wifiRssi = WiFi.RSSI();
    strlcpy(s.ipAddress, WiFi.localIP().toString().c_str(), sizeof(s.ipAddress));
  }

  s.rtcPresent = rtc.isPresent();
  s.timeSynced = timeSynced;
  s.rtcTimeInvalid = rtcTimeInvalid;
  s.now = currentTime;

  s.sensorPresent = climateSensor.isPresent();
  s.temperature = temperatureC;
  s.humidity = humidityPct;
  s.history = &history;
  s.historyIntervalMin = HISTORY_INTERVAL_MIN;

  s.batteryPresent = battery.isPresent();
  s.batteryCharging = battery.isCharging();
  s.batteryPercent = battery.percent();

  s.uptimeSec = millis() / 1000;

  uiDrawDashboard(lcd.u8g2(), s);
}

static void printSummary()
{
  Serial.println();
  Serial.println("===== ESP32 RLCD Dashboard =====");
  Serial.printf("SHTC3   : %s  T=%.2f C  RH=%.1f %%\n",
                climateSensor.isPresent() ? "OK" : "missing", temperatureC, humidityPct);
  Serial.printf("RTC     : %s  %04u-%02u-%02u %02u:%02u:%02u\n", rtc.isPresent() ? "OK" : "missing",
                currentTime.year, currentTime.month, currentTime.day,
                currentTime.hour, currentTime.minute, currentTime.second);
  Serial.printf("Battery : %.2f V (%d %%)\n", battery.voltage(), battery.percent());
  Serial.printf("WiFi    : %s\n", wifiConnected() ? WiFi.localIP().toString().c_str() : "disconnected");
}

// ============================================================
//                        SETUP / LOOP
// ============================================================
void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(200);

  if (!lcd.begin(0, U8G2_R1)) {
    Serial.println("[lcd] Frame buffer allocation failed");
  }
  lcd.u8g2().enableUTF8Print();
  uiDrawSplash(lcd.u8g2(), "Detecting hardware...");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  if (!climateSensor.begin()) {
    Serial.println("[sht] SHTC3 not detected");
  }
  initRtc();
  battery.begin();

  uiDrawSplash(lcd.u8g2(), "Syncing clock (NTP)...");
  if (wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) {
    syncTimeFromNtp();
  } else {
    Serial.println("[wifi] Could not connect");
  }
  lastNtpAttemptMs = millis();

  readSensors();
  if (climateValid()) history.push(temperatureC, humidityPct);
  lastSensorReadMs = lastHistoryMs = millis();

  printSummary();
  drawDashboard();
}

void loop()
{
  const uint32_t now = millis();

  if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSensors();
    drawDashboard();
  }

  if (now - lastHistoryMs >= minutesToMs(HISTORY_INTERVAL_MIN)) {
    lastHistoryMs = now;
    if (climateValid()) history.push(temperatureC, humidityPct);
  }

  // Periodic NTP re-sync; never blocks waiting for WiFi
  const uint32_t ntpInterval = minutesToMs(timeSynced ? NTP_RESYNC_MIN : NTP_RETRY_MIN);
  if (rtc.isPresent() && now - lastNtpAttemptMs >= ntpInterval) {
    lastNtpAttemptMs = now;
    if (wifiConnected()) syncTimeFromNtp();
    else                 wifiReconnect();
  }

  delay(LOOP_DELAY_MS);
}
