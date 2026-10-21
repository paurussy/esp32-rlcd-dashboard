/*
  ESP32 RLCD Dashboard
  --------------------
  Desk clock and indoor climate dashboard for the Waveshare ESP32-S3-RLCD-4.2.

    - Large clock from the PCF85063 RTC, kept in time via NTP
    - Date line
    - Temperature and humidity (SHTC3) with a ~22 h chart
    - Header with WiFi signal (only while connected) and battery level
    - Today's Teams/Outlook meetings pushed from a PC (pc/calendar_bridge),
      with beeps 5 minutes before and at the start
    - Pending tasks from a small web page on a PC (pc/tasks_server)
    - KEY switches screens; BOOT cycles the charts (dashboard), mutes the beeps
      (calendar) or selects the next task (tasks)
    - The calendar and tasks screens are turned on by CALENDAR_TOKEN and
      TASKS_TOKEN in secrets.h

  Built to run on little power: the screen is redrawn once per minute (or on a
  button press), the CPU light-sleeps in between, Bluetooth is never started and
  WiFi is only on during short windows (see WIFI_WINDOW_PERIOD_MIN) to take the
  calendar and tasks from the PC and, once an hour, to re-sync the clock.

  All settings live in config.h; WiFi credentials go in secrets.h
  (copy secrets.example.h). See README.md for build instructions.
*/

#include <WiFi.h>
#include <Wire.h>
#include <esp_sleep.h>

#include "config.h"
#include "src/drivers/battery.h"
#include "src/drivers/button.h"
#include "src/drivers/pcf85063.h"
#include "src/drivers/shtc3.h"
#include "src/drivers/speaker.h"
#include "src/drivers/st7305_u8g2.h"
#include "src/model/calendar.h"
#include "src/model/date_time.h"
#include "src/model/history.h"
#include "src/model/tasks.h"
#include "src/net/network.h"
#include "src/net/push_server.h"
#include "src/ui/ui.h"

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
static TaskList tasks;
static int taskSelected = 0;         // Index into tasks, moved with BOOT on the tasks screen

enum class Screen { Dashboard, Calendar, Tasks };
static Screen screen = Screen::Dashboard;

static bool rtcTimeInvalid = false;  // RTC lost power; showing compile time
static bool timeSynced = false;      // NTP succeeded at least once
static time_t lastNtpEpoch = 0;      // When (RTC time) it last did
static bool ntpFailed = false;       // Last attempt failed: retry with growing waits
static uint8_t ntpFailures = 0;      // Failed attempts in a row
static time_t ntpAttemptEpoch = 0;   // When the last one was made

static bool windowOpen = false;      // WiFi up and listening for the PC
static bool calendarServed = false;  // Received in this window
static bool tasksServed = false;
static bool windowServed = false;    // Everything enabled received in this window: don't reopen
static uint32_t closeAtMs = 0;       // Pending close after a push (0 = none)
static uint32_t windowActivityMs = 0;  // Window opened, or last push received

static bool booted = false;          // setup() finished: the screen can be redrawn
static int lastMinute = -1;         // Minute of the last RTC reading handled
static uint32_t lastClockPollMs = 0;

static ChartView chartView = ChartView::Both;
static bool alertsMuted = false;     // Meeting beeps silenced with BOOT on the calendar screen
static uint8_t mutedDay = 0;         // Day of the month it was muted: beeps come back on the next one

static Button keyButton(PIN_BUTTON_KEY);    // Switches screens
static Button bootButton(PIN_BUTTON_BOOT);  // Per-screen action: chart view / mute

// While awake with the radio on, the RTC is polled this often for the next minute
constexpr uint32_t CLOCK_POLL_MS = 250;
// Lets the HTTP response of the push reach the PC before the radio goes off
constexpr uint32_t CLOSE_DELAY_MS = 500;

// Serves the buttons (and redraws) while WiFi or NTP is being waited for.
static void serviceButtonsWhileBusy();

// ============================================================
//                          HELPERS
// ============================================================
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
  // The radio's current peaks sag the cell: a reading then would understate the charge
  if (!wifiConnected()) battery.update();
}

// Wait before the next retry: NTP_RETRY_MIN after the first failure, doubling with
// each one up to NTP_RETRY_MAX_MIN.
static uint32_t ntpRetryMin()
{
  const uint32_t shift = ntpFailures > 1 ? min<uint32_t>(ntpFailures - 1, 8) : 0;
  return min<uint32_t>(NTP_RETRY_MIN << shift, NTP_RETRY_MAX_MIN);
}

static void noteNtpAttempt(bool ok)
{
  ntpFailed = !ok;
  ntpFailures = ok ? 0 : min<int>(ntpFailures + 1, 16);
  ntpAttemptEpoch = nowEpoch();
  if (!ok) Serial.printf("[ntp] Sync failed, retrying in %lu min\n", static_cast<unsigned long>(ntpRetryMin()));
}

// Fetches the time from NTP and stores it in the RTC.
static bool syncTimeFromNtp()
{
  if (!rtc.isPresent()) return false;

  DateTime t;
  if (!ntpFetchLocalTime(t, NTP_RESPONSE_TIMEOUT_MS, serviceButtonsWhileBusy)) {
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

// ============================================================
//                         WIFI WINDOWS
// ============================================================
// Whether WiFi is allowed at all at this time of day.
static bool inActiveHours(const DateTime &t)
{
  return t.hour >= WIFI_ACTIVE_FROM_HOUR && t.hour < WIFI_ACTIVE_UNTIL_HOUR;
}

// True in the minutes of a window: the ones just before and after each
// multiple of WIFI_WINDOW_PERIOD_MIN (:59 and :00, :14 and :15, ...), for the
// slots within the active hours.
static bool windowActiveAt(const DateTime &t)
{
  const uint32_t shifted = t.hour * 60U + t.minute + WIFI_WINDOW_OPEN_BEFORE_MIN;
  if (shifted % WIFI_WINDOW_PERIOD_MIN >= WIFI_WINDOW_OPEN_BEFORE_MIN + WIFI_WINDOW_CLOSE_AFTER_MIN) return false;

  const uint32_t slot = shifted - shifted % WIFI_WINDOW_PERIOD_MIN;  // Minute of the day
  return slot >= WIFI_ACTIVE_FROM_HOUR * 60U && slot < WIFI_ACTIVE_UNTIL_HOUR * 60U;
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

// A failed sync is retried after a growing wait (see ntpRetryMin), window or not,
// but only within the active hours.
static bool ntpRetryDue()
{
  return rtc.isPresent() && ntpFailed && inActiveHours(currentTime) &&
         nowEpoch() - ntpAttemptEpoch >= static_cast<time_t>(ntpRetryMin() * 60);
}

static void closeWindow()
{
  if (PUSH_ENABLED) pushServerStop();
  wifiOff();
  windowOpen = false;
  closeAtMs = 0;
}

// Brings WiFi up, starts listening for the PC and re-syncs the clock if due.
static void openWindow()
{
  if (!PUSH_ENABLED && !ntpDue()) return;

  if (!wifiConnect(WIFI_CONNECT_TIMEOUT_MS, serviceButtonsWhileBusy)) {
    Serial.println("[wifi] Could not connect, trying again in the next window");
    wifiOff();
    if (ntpDue()) noteNtpAttempt(false);
    return;
  }
  windowOpen = true;
  windowActivityMs = millis();
  if (PUSH_ENABLED) pushServerStart();

  if (ntpDue()) syncTimeFromNtp();
  rtc.read(currentTime);
  lastMinute = currentTime.minute;  // The connection may have taken a while

  if (!PUSH_ENABLED) closeWindow();
}

// Radio on just long enough to retry a failed clock sync.
static void retryNtpOnly()
{
  if (wifiConnect(WIFI_CONNECT_TIMEOUT_MS, serviceButtonsWhileBusy)) syncTimeFromNtp();
  else noteNtpAttempt(false);
  wifiOff();
  rtc.read(currentTime);
  lastMinute = currentTime.minute;
}

// Opens the window when it starts and closes it when it ends.
static void manageWindow()
{
  if (!windowActiveAt(currentTime)) {
    windowServed = calendarServed = tasksServed = false;
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
  s.chartView = chartView;
  s.alertsMuted = alertsMuted;

  s.batteryPresent = battery.isPresent();
  s.batteryCharging = battery.isCharging();
  s.batteryPercent = battery.percent();

  s.uptimeSec = millis() / 1000;

  s.pageCount = PAGE_COUNT;
  s.page = screen == Screen::Dashboard ? 0 : screen == Screen::Calendar ? 1 : PAGE_COUNT - 1;
  return s;
}

static void drawCalendar()
{
  CalendarView c;
  c.now = nowEpoch();
  c.count = calendar.upcoming(c.now, c.events, CALENDAR_VISIBLE_EVENTS);
  c.hasData = calendar.hasData();
  c.lastUpdate = calendar.lastUpdate();
  c.stale = c.hasData && inActiveHours(currentTime) && c.now - c.lastUpdate > static_cast<time_t>(CALENDAR_STALE_MIN * 60);

  uiDrawCalendar(lcd.u8g2(), dashboardState(), c);
}

static void drawTasks()
{
  TasksView t;
  t.now = nowEpoch();
  t.list = &tasks;
  t.selected = taskSelected;
  t.hasData = tasks.hasData();
  t.lastUpdate = tasks.lastUpdate();
  t.stale = t.hasData && inActiveHours(currentTime) && t.now - t.lastUpdate > static_cast<time_t>(TASKS_STALE_MIN * 60);

  uiDrawTasks(lcd.u8g2(), dashboardState(), t);
}

static void drawScreen()
{
  if (CALENDAR_ENABLED && screen == Screen::Calendar)  drawCalendar();
  else if (TASKS_ENABLED && screen == Screen::Tasks)  drawTasks();
  else                                                 uiDrawDashboard(lcd.u8g2(), dashboardState());
}

// The screen after `screen`, skipping the ones that are not enabled.
static Screen nextScreen()
{
  switch (screen) {
    case Screen::Dashboard: return CALENDAR_ENABLED ? Screen::Calendar : TASKS_ENABLED ? Screen::Tasks : Screen::Dashboard;
    case Screen::Calendar:  return TASKS_ENABLED ? Screen::Tasks : Screen::Dashboard;
    default:                return Screen::Dashboard;
  }
}

// Beeps (and jumps to the calendar) when a meeting is about to start or starts.
static void checkCalendarAlerts()
{
  if (rtcTimeInvalid && !timeSynced) return;  // Clock not trustworthy yet

  const CalendarEvent *event = nullptr;
  CalendarAlert alert = calendar.pollAlerts(nowEpoch(), &event);
  if (alert == CalendarAlert::None) return;

  Serial.printf("[cal] %s: %s%s\n", alert == CalendarAlert::Early ? "Starting soon" : "Starting now",
                event->subject, alertsMuted ? " (muted)" : "");
  if (!alertsMuted) speaker.beep(alert == CalendarAlert::Early ? BeepPattern::Early : BeepPattern::Start);
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

// KEY silences a beep in progress; otherwise it switches screens.
static void handleKey()
{
  if (speaker.isPlaying()) speaker.stop();
  else screen = nextScreen();
}

// Keeps the selection on the same task after a push changed the list, else on the
// same position.
static void keepTaskSelection(uint32_t selectedId)
{
  for (int i = 0; i < tasks.count(); i++) {
    if (tasks.at(i).id == selectedId) {
      taskSelected = i;
      return;
    }
  }
  taskSelected = constrain(taskSelected, 0, max(tasks.count() - 1, 0));
}

// BOOT on the tasks screen selects the next task (the list scrolls to follow it).
static void handleBootTasks()
{
  if (tasks.count() == 0) return;
  taskSelected = (constrain(taskSelected, 0, tasks.count() - 1) + 1) % tasks.count();
}

// BOOT, per screen. Tasks: see handleBootTasks. Calendar: mutes/unmutes the meeting
// beeps (until the next day). Dashboard: cycles the charts: both, temperature only,
// humidity only. There a long press counts as a click.
static void handleBoot(Gesture gesture)
{
  if (TASKS_ENABLED && screen == Screen::Tasks) {
    handleBootTasks();
    return;
  }
  if (CALENDAR_ENABLED && screen == Screen::Calendar) {
    alertsMuted = !alertsMuted;
    mutedDay = currentTime.day;
    if (alertsMuted && speaker.isPlaying()) speaker.stop();
    Serial.printf("[cal] Beeps %s\n", alertsMuted ? "muted" : "on");
    return;
  }
  switch (chartView) {
    case ChartView::Both:        chartView = ChartView::Temperature; break;
    case ChartView::Temperature: chartView = ChartView::Humidity;    break;
    case ChartView::Humidity:    chartView = ChartView::Both;        break;
  }
}

// Reads both buttons; true if one did something that needs a redraw.
static bool serviceButtons()
{
  bool changed = false;
  if (PAGE_COUNT > 1 && keyButton.pressed()) {
    handleKey();
    changed = true;
  }
  const Gesture gesture = bootButton.poll(false);
  if (gesture != Gesture::None) {
    handleBoot(gesture);
    changed = true;
  }
  return changed;
}

static void serviceButtonsWhileBusy()
{
  if (!booted) return;  // Splash still up: a latched press is served once the loop runs
  if (serviceButtons()) drawScreen();
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
  if (TASKS_ENABLED) {
    Serial.printf("Tasks   : http://%s.local/tasks (WiFi windows every %lu min)\n", MDNS_HOSTNAME,
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
  battery.update();  // Before WiFi starts: the radio sags the cell
  rtc.read(currentTime);  // So a failed NTP attempt below is timestamped with the real time
  bootButton.begin();
  if (PAGE_COUNT > 1) keyButton.begin();
  if (!CALENDAR_ENABLED) {
    speaker.shutdown();  // Never used: keep the amplifier and codec from drawing power
  } else if (!speaker.begin(AUDIO_SAMPLE_RATE, BEEP_FREQ_HZ, BEEP_VOLUME_PCT)) {
    Serial.println("[audio] ES8311 not detected");
  }

  uiDrawSplash(lcd.u8g2(), "Syncing clock (NTP)...");
  if (wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) {
    syncTimeFromNtp();
  } else {
    Serial.println("[wifi] Could not connect");
    if (wifiConfigured()) noteNtpAttempt(false);  // Without credentials there is nothing to retry
  }
  readSensors();
  if (PUSH_ENABLED && wifiConfigured()) {
    pushServerBegin(CALENDAR_ENABLED ? &calendar : nullptr, TASKS_ENABLED ? &tasks : nullptr, nowEpoch);
  }

  // Booted inside a window: stay up for the PC. Otherwise radio off until the next one.
  if (PUSH_ENABLED && wifiConnected() && windowActiveAt(currentTime)) {
    pushServerStart();
    windowOpen = true;
    windowActivityMs = millis();
  } else {
    wifiOff();
  }

  if (climateValid()) history.add(temperatureC, humidityPct);
  lastMinute = currentTime.minute;

  printSummary();
  drawScreen();
  booted = true;
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
  bootButton.armWake();
  if (PAGE_COUNT > 1) keyButton.armWake();
  esp_light_sleep_start();
  bootButton.afterWake();
  if (PAGE_COUNT > 1) keyButton.afterWake();
  lcd.holdPins(false);
  return esp_sleep_get_wakeup_cause();
}

// Radio on, a beep playing or a button still down: stay awake and poll.
static bool mustStayAwake()
{
  return windowOpen || speaker.isPlaying() || !bootButton.idle() ||
         (PAGE_COUNT > 1 && !keyButton.idle());
}

// New minute: refresh readings, alerts and the WiFi window.
static void onMinute()
{
  readSensors();
  if (climateValid()) history.add(temperatureC, humidityPct);
  if (alertsMuted && currentTime.day != mutedDay) alertsMuted = false;  // New day: beeps back on
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

  if (serviceButtons()) redraw = true;

  // Once everything enabled has arrived the window ends shortly after; alerts that
  // a push brings are not left to the next minute
  if (windowOpen && PUSH_ENABLED) {
    const uint32_t selectedId = taskSelected < tasks.count() ? tasks.at(taskSelected).id : 0;
    const uint8_t pushed = pushServerLoop();
    if (pushed != 0) {
      windowActivityMs = millis();
      rtc.read(currentTime);
      if (pushed & PUSH_CALENDAR) {
        checkCalendarAlerts();
        calendarServed = true;
      }
      if (pushed & PUSH_TASKS) {
        keepTaskSelection(selectedId);
        tasksServed = true;
      }
      if ((!CALENDAR_ENABLED || calendarServed) && (!TASKS_ENABLED || tasksServed)) {
        windowServed = true;
        closeAtMs = millis() + CLOSE_DELAY_MS;
      }
      redraw = true;
    }
  }
  if (closeAtMs != 0 && static_cast<int32_t>(millis() - closeAtMs) >= 0) {
    closeWindow();
    redraw = true;  // The WiFi icon goes away
  } else if (windowOpen && closeAtMs == 0 && millis() - windowActivityMs >= WIFI_WINDOW_IDLE_SEC * 1000UL) {
    Serial.println("[wifi] No push from the PC, closing the window");
    windowServed = true;  // Do not reopen it in the next minute of this window
    closeWindow();
    redraw = true;
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
