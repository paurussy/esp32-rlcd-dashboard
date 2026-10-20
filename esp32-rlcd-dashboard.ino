/*
  ESP32 RLCD Dashboard
  --------------------
  Desk clock and indoor climate dashboard for the Waveshare ESP32-S3-RLCD-4.2.

    - Large clock from the PCF85063 RTC, kept in time via NTP
    - Date line
    - Temperature and humidity (SHTC3) with a ~22 h chart
    - Header with WiFi signal (only while connected) and battery level
    - Today's Teams/Outlook meetings pushed from a PC (tools/calendar-bridge),
      with beeps 5 minutes before and at the start; KEY switches screens

  Built to run on little power: the screen is redrawn once per minute (or on a
  KEY press), the CPU light-sleeps in between, Bluetooth is never started and
  WiFi is only on during short windows (see WIFI_WINDOW_PERIOD_MIN) to take the
  calendar from the PC and, once an hour, to re-sync the clock.

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
static time_t lastNtpEpoch = 0;      // When (RTC time) it last did
static bool ntpFailed = false;       // Last attempt failed: retry every NTP_RETRY_MIN
static time_t ntpAttemptEpoch = 0;   // When it was made

static bool windowOpen = false;      // WiFi up and listening for the PC
static bool windowServed = false;    // Calendar received in this window: don't reopen
static uint32_t closeAtMs = 0;       // Pending close after a push (0 = none)

static int lastMinute = -1;          // Minute of the last RTC reading handled
static uint32_t lastHistoryMs = 0;
static uint32_t lastClockPollMs = 0;

static bool keyStableLevel = HIGH;
static bool keyLastLevel = HIGH;
static uint32_t keyChangedMs = 0;

// While awake with the radio on, the RTC is polled this often for the next minute
constexpr uint32_t CLOCK_POLL_MS = 250;
// Lets the HTTP response of the push reach the PC before the radio goes off
constexpr uint32_t CLOSE_DELAY_MS = 500;

// ============================================================
//                          HELPERS
// ============================================================
static constexpr uint32_t minutesToMs(uint32_t minutes) { return minutes * 60UL * 1000UL; }

// Current Unix time, from the last RTC reading.
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

static void noteNtpAttempt(bool ok)
{
  ntpFailed = !ok;
  ntpAttemptEpoch = nowEpoch();
  if (!ok) Serial.printf("[ntp] Sync failed, retrying in %lu min\n", static_cast<unsigned long>(NTP_RETRY_MIN));
}

// Fetches the time from NTP and stores it in the RTC.
static bool syncTimeFromNtp()
{
  if (!rtc.isPresent()) return false;

  DateTime t;
  if (!ntpFetchLocalTime(t, NTP_RESPONSE_TIMEOUT_MS)) {
    Serial.println("[ntp] No response from time server");
    noteNtpAttempt(false);
    return false;
  }

  rtc.write(t);
  currentTime = t;
  lastNtpEpoch = nowEpoch();
  rtcTimeInvalid = false;
  timeSynced = true;
  noteNtpAttempt(true);
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

// ============================================================
//                         WIFI WINDOWS
// ============================================================
// True in the minutes of a window: the ones just before and after each
// multiple of WIFI_WINDOW_PERIOD_MIN (:59 and :00, :14 and :15, ...).
static bool windowActiveAt(uint8_t minute)
{
  return (minute + WIFI_WINDOW_OPEN_BEFORE_MIN) % WIFI_WINDOW_PERIOD_MIN <
         WIFI_WINDOW_OPEN_BEFORE_MIN + WIFI_WINDOW_CLOSE_AFTER_MIN;
}

// Whether the clock needs a sync in this window. Windows are a period apart, so
// the one closest to the deadline counts: without the slack, a sync made at
// :59:30 would not look due 59 minutes later.
static bool ntpDue()
{
  if (!rtc.isPresent()) return false;
  if (!timeSynced) return true;
  const time_t elapsed = nowEpoch() - lastNtpEpoch;
  return elapsed + static_cast<time_t>(WIFI_WINDOW_PERIOD_MIN * 30) >=
         static_cast<time_t>(NTP_RESYNC_MIN * 60);
}

// A failed sync is retried every NTP_RETRY_MIN minutes, window or not.
static bool ntpRetryDue()
{
  return rtc.isPresent() && ntpFailed &&
         nowEpoch() - ntpAttemptEpoch >= static_cast<time_t>(NTP_RETRY_MIN * 60);
}

static void closeWindow()
{
  if (CALENDAR_ENABLED) calendarServerStop();
  wifiOff();
  windowOpen = false;
  closeAtMs = 0;
}

// Brings WiFi up, starts listening for the PC and re-syncs the clock if due.
static void openWindow()
{
  if (!CALENDAR_ENABLED && !ntpDue()) return;

  if (!wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println("[wifi] Could not connect, trying again in the next window");
    wifiOff();
    if (ntpDue()) noteNtpAttempt(false);
    return;
  }
  windowOpen = true;
  if (CALENDAR_ENABLED) calendarServerStart();

  if (ntpDue()) syncTimeFromNtp();
  rtc.read(currentTime);
  lastMinute = currentTime.minute;  // The connection may have taken a while

  if (!CALENDAR_ENABLED) closeWindow();
}

// Radio on just long enough to retry a failed clock sync.
static void retryNtpOnly()
{
  if (wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) syncTimeFromNtp();
  else noteNtpAttempt(false);
  wifiOff();
  rtc.read(currentTime);
  lastMinute = currentTime.minute;
}

// Opens the window when it starts and closes it when it ends.
static void manageWindow()
{
  if (!windowActiveAt(currentTime.minute)) {
    windowServed = false;
    if (windowOpen) closeWindow();
    else if (ntpRetryDue()) retryNtpOnly();
    return;
  }
  if (!windowOpen && !windowServed) openWindow();
}

// ============================================================
//                       SCREENS AND KEYS
// ============================================================
static DashboardState dashboardState()
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
  s.lastNtpSync = timeSynced ? lastNtpEpoch : 0;
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
  bool level = digitalRead(PIN_BUTTON_KEY);
  if (level != keyLastLevel) {
    keyLastLevel = level;
    keyChangedMs = millis();
  }
  if (level == keyStableLevel || millis() - keyChangedMs < BUTTON_DEBOUNCE_MS) return false;

  keyStableLevel = level;
  return level == LOW;
}

// Released and settled: safe to sleep (the wake-up on a low level would fire at once otherwise).
static bool keyIdle()
{
  return digitalRead(PIN_BUTTON_KEY) == HIGH && keyStableLevel == HIGH;
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
  Serial.printf("WiFi    : %s\n", wifiConnected() ? WiFi.localIP().toString().c_str() : "off");
  if (CALENDAR_ENABLED) {
    Serial.printf("Speaker : %s\n", speaker.isPresent() ? "OK" : "missing");
    Serial.printf("Calendar: http://%s.local/calendar (WiFi windows every %lu min)\n", MDNS_HOSTNAME,
                  static_cast<unsigned long>(WIFI_WINDOW_PERIOD_MIN));
  }
}

// ============================================================
//                        SETUP / LOOP
// ============================================================
void setup()
{
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  btStop();  // Bluetooth is never used

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
    gpio_wakeup_enable(static_cast<gpio_num_t>(PIN_BUTTON_KEY), GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    if (!speaker.begin(AUDIO_SAMPLE_RATE, BEEP_FREQ_HZ, BEEP_VOLUME_PCT)) {
      Serial.println("[audio] ES8311 not detected");
    }
  }

  uiDrawSplash(lcd.u8g2(), "Syncing clock (NTP)...");
  if (wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) {
    syncTimeFromNtp();
  } else {
    Serial.println("[wifi] Could not connect");
    rtc.read(currentTime);
    noteNtpAttempt(false);
  }
  readSensors();
  if (CALENDAR_ENABLED && wifiConfigured()) calendarServerBegin(calendar, nowEpoch);

  // Booted inside a window: stay up for the PC. Otherwise radio off until the next one.
  if (CALENDAR_ENABLED && wifiConnected() && windowActiveAt(currentTime.minute)) {
    calendarServerStart();
    windowOpen = true;
  } else {
    wifiOff();
  }

  if (climateValid()) history.push(temperatureC, humidityPct);
  lastHistoryMs = millis();
  lastMinute = currentTime.minute;

  printSummary();
  drawScreen();
  if (CALENDAR_ENABLED && BEEP_ON_BOOT) speaker.beep(BeepPattern::Test);
}

// Light-sleeps until just after the next minute starts, or until KEY is pressed.
static esp_sleep_wakeup_cause_t sleepUntilNextMinute()
{
  rtc.read(currentTime);  // Fresh value: the cycle may have taken a few seconds
  uint64_t sleepMs = (60 - currentTime.second) * 1000ULL + SLEEP_WAKE_MARGIN_MS;

  Serial.flush();
  lcd.holdPins(true);
  esp_sleep_enable_timer_wakeup(sleepMs * 1000ULL);
  esp_light_sleep_start();
  lcd.holdPins(false);
  return esp_sleep_get_wakeup_cause();
}

// Radio on, a beep playing or KEY still down: stay awake and poll.
static bool mustStayAwake()
{
  return windowOpen || speaker.isPlaying() || (CALENDAR_ENABLED && !keyIdle());
}

// New minute: refresh readings, alerts and the WiFi window.
static void onMinute()
{
  readSensors();
  updateHistory(millis());  // millis() keeps counting during light sleep
  if (CALENDAR_ENABLED) {
    checkCalendarAlerts();
    leaveCalendarWhenDone();
  }
  manageWindow();
}

void loop()
{
  bool redraw = false;
  bool timerWake = false;

  if (!mustStayAwake()) timerWake = sleepUntilNextMinute() == ESP_SLEEP_WAKEUP_TIMER;

  if (CALENDAR_ENABLED && keyPressed()) {
    handleKey();
    redraw = true;
  }

  // A push ends the window shortly after; alerts that its events bring are not left to the next minute
  if (windowOpen && CALENDAR_ENABLED && calendarServerLoop()) {
    rtc.read(currentTime);
    checkCalendarAlerts();
    windowServed = true;
    closeAtMs = millis() + CLOSE_DELAY_MS;
    redraw = true;
  }
  if (closeAtMs != 0 && static_cast<int32_t>(millis() - closeAtMs) >= 0) {
    closeWindow();
    redraw = true;  // The WiFi icon goes away
  }

  const uint32_t now = millis();
  if (timerWake || now - lastClockPollMs >= CLOCK_POLL_MS) {
    lastClockPollMs = now;
    rtc.read(currentTime);
    // Light sleep timers are not exact: only act when the RTC really shows a new minute
    if (currentTime.minute != lastMinute || (timerWake && !rtc.isPresent())) {
      lastMinute = currentTime.minute;
      onMinute();
      redraw = true;
    }
  }

  if (redraw) drawScreen();
  if (mustStayAwake()) delay(LOOP_DELAY_MS);
}
