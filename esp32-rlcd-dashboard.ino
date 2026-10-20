/*
  ESP32 RLCD Dashboard
  --------------------
  Desk clock and indoor climate dashboard for the Waveshare ESP32-S3-RLCD-4.2.

    - Large clock from the PCF85063 RTC, kept in time via NTP
    - Date line
    - Temperature and humidity (SHTC3) with a ~22 h chart
    - Header with WiFi signal and battery level
    - Today's Teams/Outlook meetings pushed from a PC (tools/calendar-bridge),
      with beeps 5 minutes before and at the start; KEY switches screens
    - Optional low-power mode (LOW_POWER_MODE in config.h)

  All settings live in config.h; WiFi credentials go in secrets.h
  (copy secrets.example.h). See README.md for build instructions.
*/

#include <WiFi.h>
#include <Wire.h>
#include <esp_sleep.h>

#include "config.h"
#include "src/battery.h"
#include "src/calendar.h"
#include "src/calendar_server.h"
#include "src/date_time.h"
#include "src/history.h"
#include "src/network.h"
#include "src/pcf85063.h"
#include "src/shtc3.h"
#include "src/speaker.h"
#include "src/st7305_u8g2.h"
#include "src/ui.h"

// ---------------- Hardware ----------------
static ST7305_U8g2 lcd(PIN_LCD_SCK, PIN_LCD_MOSI, PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_RST);
static Shtc3 climateSensor(Wire, I2C_ADDR_SHTC3);
static Pcf85063 rtc(Wire, I2C_ADDR_PCF85063);
static Battery battery(PIN_BATTERY_ADC);
static Speaker speaker(Wire, I2C_ADDR_ES8311,
                       {PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_AUDIO_PA});

// ---------------- State ----------------
static History history;
static DateTime currentTime;
static float temperatureC = NAN;
static float humidityPct = NAN;
static Calendar calendar;

enum class Screen { Dashboard, Calendar };
static Screen screen = Screen::Dashboard;

static bool rtcTimeInvalid = false;  // RTC lost power; showing compile time
static bool timeSynced = false;      // NTP succeeded at least once
static bool lastSyncOk = false;      // Last NTP attempt succeeded (else retry sooner)

static uint32_t lastSensorReadMs = 0;
static uint32_t lastHistoryMs = 0;
static uint32_t lastNtpAttemptMs = 0;

// ============================================================
//                          HELPERS
// ============================================================
static constexpr uint32_t minutesToMs(uint32_t minutes) { return minutes * 60UL * 1000UL; }

// Current Unix time, from the RTC reading taken once per second.
static time_t nowEpoch() { return dateTimeToEpoch(currentTime); }

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

static void updateHistory(uint32_t now)
{
  if (now - lastHistoryMs < minutesToMs(HISTORY_INTERVAL_MIN)) return;

  lastHistoryMs = now;
  if (climateValid()) history.push(temperatureC, humidityPct);
}

// Periodic NTP re-sync: every NTP_RESYNC_MIN, or NTP_RETRY_MIN after a failure.
// In normal mode it never blocks waiting for WiFi; in low-power mode the radio
// is switched on just for the sync.
static void resyncTimeIfDue(uint32_t now)
{
  const uint32_t interval = minutesToMs(lastSyncOk ? NTP_RESYNC_MIN : NTP_RETRY_MIN);
  if (!rtc.isPresent() || now - lastNtpAttemptMs < interval) return;

  lastNtpAttemptMs = now;
  if (LOW_POWER_MODE) {
    lastSyncOk = wifiConnect(WIFI_CONNECT_TIMEOUT_MS) && syncTimeFromNtp();
    wifiOff();
  } else if (wifiConnected()) {
    lastSyncOk = syncTimeFromNtp();
  } else {
    lastSyncOk = false;
    wifiReconnect();
  }
  if (!lastSyncOk) Serial.printf("[ntp] Sync failed, retrying in %lu min\n", static_cast<unsigned long>(NTP_RETRY_MIN));
}

// Light-sleeps until just after the next minute starts.
static void sleepUntilNextMinute()
{
  rtc.read(currentTime);  // Fresh value: the cycle may have taken a few seconds
  uint64_t sleepMs = (60 - currentTime.second) * 1000ULL + LOW_POWER_WAKE_MARGIN_MS;

  Serial.flush();
  lcd.holdPins(true);
  esp_sleep_enable_timer_wakeup(sleepMs * 1000ULL);
  esp_light_sleep_start();
  lcd.holdPins(false);
}

static DashboardState dashboardState()
{
  DashboardState s;

  s.showSeconds = !LOW_POWER_MODE;
  s.wifiSleeping = LOW_POWER_MODE;
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

  s.pageCount = CALENDAR_ENABLED ? 2 : 1;
  s.page = screen == Screen::Calendar ? 1 : 0;
  return s;
}

static void drawCalendar()
{
  CalendarView c;
  c.now = nowEpoch();
  c.count = calendar.upcoming(c.now, c.events, CALENDAR_VISIBLE_EVENTS);
  c.hasData = calendar.hasData();
  c.lastUpdate = calendar.lastUpdate();
  c.stale = c.hasData && c.now - c.lastUpdate > static_cast<time_t>(CALENDAR_STALE_MIN * 60);

  uiDrawCalendar(lcd.u8g2(), dashboardState(), c);
}

static void drawScreen()
{
  if (CALENDAR_ENABLED && screen == Screen::Calendar) drawCalendar();
  else                                                 uiDrawDashboard(lcd.u8g2(), dashboardState());
}

// Beeps (and jumps to the calendar) when a meeting is about to start or starts.
static void checkCalendarAlerts()
{
  if (rtcTimeInvalid && !timeSynced) return;  // Clock not trustworthy yet

  const CalendarEvent *event = nullptr;
  CalendarAlert alert = calendar.pollAlerts(nowEpoch(), &event);
  if (alert == CalendarAlert::None) return;

  Serial.printf("[cal] %s: %s\n", alert == CalendarAlert::Early ? "Starting soon" : "Starting now",
                event->subject);
  speaker.beep(alert == CalendarAlert::Early ? BeepPattern::Early : BeepPattern::Start);
  screen = Screen::Calendar;
}

// Back to the dashboard once, when the last meeting of the day ends.
// Edge-triggered: going back to the empty calendar with KEY stays there.
static void leaveCalendarWhenDone()
{
  static int lastUpcoming = 0;

  const CalendarEvent *unused[1];
  int upcoming = calendar.upcoming(nowEpoch(), unused, 1);
  if (lastUpcoming > 0 && upcoming == 0 && screen == Screen::Calendar) {
    Serial.println("[cal] No more meetings, back to the dashboard");
    screen = Screen::Dashboard;
  }
  lastUpcoming = upcoming;
}

// Debounced KEY press (falling edge).
static bool keyPressed()
{
  static bool stableLevel = HIGH;
  static bool lastLevel = HIGH;
  static uint32_t changedMs = 0;

  bool level = digitalRead(PIN_BUTTON_KEY);
  if (level != lastLevel) {
    lastLevel = level;
    changedMs = millis();
  }
  if (level == stableLevel || millis() - changedMs < BUTTON_DEBOUNCE_MS) return false;

  stableLevel = level;
  return level == LOW;
}

// KEY silences a beep in progress; otherwise it switches screens.
static void handleKey()
{
  if (speaker.isPlaying()) speaker.stop();
  else screen = screen == Screen::Dashboard ? Screen::Calendar : Screen::Dashboard;
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
  if (CALENDAR_ENABLED) {
    Serial.printf("Speaker : %s\n", speaker.isPresent() ? "OK" : "missing");
    Serial.printf("Calendar: http://%s.local/calendar\n", MDNS_HOSTNAME);
  }
}

// ============================================================
//                        SETUP / LOOP
// ============================================================
void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(200);

  // Local time zone for epoch conversions, even before NTP succeeds
  setenv("TZ", TIMEZONE, 1);
  tzset();

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
  if (CALENDAR_ENABLED) {
    pinMode(PIN_BUTTON_KEY, INPUT_PULLUP);
    if (!speaker.begin(AUDIO_SAMPLE_RATE, BEEP_FREQ_HZ, BEEP_VOLUME_PCT)) {
      Serial.println("[audio] ES8311 not detected");
    }
  }

  uiDrawSplash(lcd.u8g2(), "Syncing clock (NTP)...");
  if (wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) {
    lastSyncOk = syncTimeFromNtp();
  } else {
    Serial.println("[wifi] Could not connect");
  }
  if (LOW_POWER_MODE) wifiOff();
  if (CALENDAR_ENABLED && wifiConfigured()) calendarServerBegin(calendar, nowEpoch);
  lastNtpAttemptMs = millis();

  readSensors();
  if (climateValid()) history.push(temperatureC, humidityPct);
  lastSensorReadMs = lastHistoryMs = millis();

  printSummary();
  drawScreen();
  if (CALENDAR_ENABLED && BEEP_ON_BOOT) speaker.beep(BeepPattern::Test);
}

// Low-power mode: one full cycle per minute, sleeping in between.
static void lowPowerLoop()
{
  sleepUntilNextMinute();

  const uint32_t now = millis();  // millis() keeps counting during light sleep
  readSensors();
  updateHistory(now);
  resyncTimeIfDue(now);
  drawScreen();
}

// Normal mode: refresh every second, or right away on a key press or calendar push.
static void normalLoop()
{
  const uint32_t now = millis();
  bool redraw = false;

  if (CALENDAR_ENABLED) {
    if (calendarServerLoop()) redraw = true;
    if (keyPressed()) {
      handleKey();
      redraw = true;
    }
  }
  if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSensors();
    if (CALENDAR_ENABLED) {
      checkCalendarAlerts();
      leaveCalendarWhenDone();
    }
    redraw = true;
  }
  if (redraw) drawScreen();
  updateHistory(now);
  resyncTimeIfDue(now);

  delay(LOOP_DELAY_MS);
}

void loop()
{
  if (LOW_POWER_MODE) lowPowerLoop();
  else                normalLoop();
}
