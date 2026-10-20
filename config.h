/*
  config.h - Project-wide configuration
  -------------------------------------
  Every tunable value (pins, timings, thresholds, time zone...) lives here.
  WiFi credentials are kept out of version control in secrets.h.
*/
#pragma once

#include <stdint.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h not found: copy secrets.example.h to secrets.h and set your WiFi credentials."
#endif

// Older secrets.h files predate the calendar: an empty token disables the check.
#ifndef CALENDAR_TOKEN
#define CALENDAR_TOKEN ""
#endif

// ============================================================
//                          PINOUT
// ============================================================
// Taken from the official Waveshare repository:
// https://github.com/waveshareteam/ESP32-S3-RLCD-4.2

// ST7305 reflective LCD (SPI)
constexpr int PIN_LCD_SCK  = 11;
constexpr int PIN_LCD_MOSI = 12;
constexpr int PIN_LCD_DC   = 5;
constexpr int PIN_LCD_CS   = 40;
constexpr int PIN_LCD_RST  = 41;

// I2C bus (SHTC3, PCF85063, audio codecs)
constexpr int PIN_I2C_SDA = 13;
constexpr int PIN_I2C_SCL = 14;

// Battery voltage sense (behind a 1/3 resistor divider)
constexpr int PIN_BATTERY_ADC = 4;

// ES8311 audio codec (I2S) and speaker amplifier enable
constexpr int PIN_I2S_MCLK = 16;
constexpr int PIN_I2S_BCLK = 9;
constexpr int PIN_I2S_WS   = 45;
constexpr int PIN_I2S_DOUT = 8;
constexpr int PIN_AUDIO_PA = 46;

// KEY button (active low). BOOT (GPIO 0) is left alone.
constexpr int PIN_BUTTON_KEY = 18;

// ============================================================
//                        PERIPHERALS
// ============================================================
constexpr uint32_t SERIAL_BAUD   = 115200;
constexpr uint32_t I2C_FREQ_HZ   = 400000;

constexpr uint8_t I2C_ADDR_SHTC3    = 0x70;  // Temperature / humidity sensor
constexpr uint8_t I2C_ADDR_PCF85063 = 0x51;  // Real-time clock
constexpr uint8_t I2C_ADDR_ES8311   = 0x18;  // Audio codec (speaker)

// ============================================================
//                       NETWORK & TIME
// ============================================================
constexpr const char *NTP_SERVER_PRIMARY   = "pool.ntp.org";
constexpr const char *NTP_SERVER_SECONDARY = "time.google.com";

// POSIX TZ string. Default: Europe/Madrid (CET/CEST with automatic DST).
constexpr const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_RESPONSE_TIMEOUT_MS = 8000;

// WiFi is off except during short windows, one every WIFI_WINDOW_PERIOD_MIN
// minutes: it opens at minute 14, 29, 44, 59 (period - 1) and closes at 01, 16,
// 31, 46 (or as soon as the work is done). The PC bridge pushes the calendar
// inside the window (see tools/calendar-bridge/).
constexpr uint32_t WIFI_WINDOW_PERIOD_MIN = 15;
constexpr uint32_t WIFI_WINDOW_OPEN_BEFORE_MIN = 1;   // Opens this many minutes before the slot
constexpr uint32_t WIFI_WINDOW_CLOSE_AFTER_MIN = 1;   // Closes this many minutes after it

// The window that finds the last NTP sync older than NTP_RESYNC_MIN also
// re-syncs the RTC. After a failed attempt WiFi is switched on briefly just for
// the sync every NTP_RETRY_MIN minutes, also outside the windows.
constexpr uint32_t NTP_RESYNC_MIN = 60;
constexpr uint32_t NTP_RETRY_MIN  = 5;

// ============================================================
//                          SENSORS
// ============================================================
// Compensates for the board's self-heating (degrees C). The official Waveshare
// example uses -4 with the chip always awake at full speed; with light sleep
// and WiFi mostly off the board runs much cooler, so this is an estimate.
// Tune it against a trusted thermometer: offset = reference - raw reading.
constexpr float TEMP_OFFSET_C = -1.5f;

// One chart sample every HISTORY_INTERVAL_MIN minutes.
// HISTORY_SIZE points fill the chart (~2 px each): 88 x 15 min = 22 h.
constexpr uint32_t HISTORY_INTERVAL_MIN = 15;
constexpr int      HISTORY_SIZE         = 88;

// ============================================================
//                          BATTERY
// ============================================================
constexpr float BATTERY_DIVIDER_RATIO = 3.0f;   // V_battery = V_pin * ratio
constexpr int   BATTERY_ADC_SAMPLES   = 16;     // Averaged per reading
constexpr float BATTERY_SMOOTHING     = 0.3f;   // EMA weight of each new reading (one per minute)
constexpr float BATTERY_ABSENT_BELOW_V = 2.5f;  // Below this: USB power only
constexpr float BATTERY_FULL_ABOVE_V   = 4.15f; // Above this: charging / charged

// ============================================================
//                    CALENDAR (TEAMS / OUTLOOK)
// ============================================================
// A PC program (tools/calendar-bridge/) reads today's Outlook calendar and
// pushes it to http://<MDNS_HOSTNAME>.local/calendar. CALENDAR_TOKEN is in secrets.h.
// Also enables the speaker (meeting beeps) and the KEY button (switch screens).
// The board only listens during the WiFi windows, so the PC must push inside them.
constexpr bool CALENDAR_ENABLED = true;

constexpr const char *MDNS_HOSTNAME = "rlcd-dashboard";
constexpr uint16_t CALENDAR_HTTP_PORT = 80;

constexpr int      CALENDAR_MAX_EVENTS       = 16;  // Kept in RAM
constexpr int      CALENDAR_VISIBLE_EVENTS   = 5;   // Current event + upcoming ones
constexpr uint32_t CALENDAR_ALERT_EARLY_MIN  = 5;   // First beep, minutes before start
constexpr uint32_t CALENDAR_ALERT_LATE_SEC   = 60;  // Start alerts missed by more than this stay silent
constexpr uint32_t CALENDAR_STALE_MIN        = 35;  // Warn when the PC hasn't pushed for this long
                                                     // (two missed 15 min windows)

// Beeps
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint32_t BEEP_FREQ_HZ      = 1000;
constexpr int      BEEP_VOLUME_PCT   = 60;     // Sample amplitude, 0..100
constexpr bool     BEEP_ON_BOOT      = true;   // Short test beep after startup

constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;

// ============================================================
//                            POWER
// ============================================================
// The screen refreshes once per minute (or on a KEY press), the CPU
// light-sleeps in between (the reflective LCD keeps the image) and Bluetooth
// is never started. USB serial output is interrupted while the board sleeps.
constexpr uint32_t CPU_FREQ_MHZ = 80;

// Wake up slightly after the minute rolls over, so the RTC already shows it.
constexpr uint32_t SLEEP_WAKE_MARGIN_MS = 200;

// ============================================================
//                          MAIN LOOP
// ============================================================
// Pace of the loop while awake (WiFi window open, beep playing, KEY held).
constexpr uint32_t LOOP_DELAY_MS = 20;
