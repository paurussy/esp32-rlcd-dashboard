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

// ============================================================
//                        PERIPHERALS
// ============================================================
constexpr uint32_t SERIAL_BAUD   = 115200;
constexpr uint32_t I2C_FREQ_HZ   = 400000;

constexpr uint8_t I2C_ADDR_SHTC3    = 0x70;  // Temperature / humidity sensor
constexpr uint8_t I2C_ADDR_PCF85063 = 0x51;  // Real-time clock

// ============================================================
//                       NETWORK & TIME
// ============================================================
constexpr const char *NTP_SERVER_PRIMARY   = "pool.ntp.org";
constexpr const char *NTP_SERVER_SECONDARY = "time.google.com";

// POSIX TZ string. Default: Europe/Madrid (CET/CEST with automatic DST).
constexpr const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_RESPONSE_TIMEOUT_MS = 8000;

// Re-sync the RTC with NTP every NTP_RESYNC_MIN minutes;
// retry every NTP_RETRY_MIN minutes while it has never succeeded.
constexpr uint32_t NTP_RESYNC_MIN = 60;
constexpr uint32_t NTP_RETRY_MIN  = 5;

// ============================================================
//                          SENSORS
// ============================================================
// Compensates for the board's self-heating (degrees C), as in the official
// Waveshare example. Tune it against a trusted thermometer.
constexpr float TEMP_OFFSET_C = -4.0f;

constexpr uint32_t SENSOR_READ_INTERVAL_MS = 1000;

// One chart sample every HISTORY_INTERVAL_MIN minutes.
// HISTORY_SIZE points fill the chart (~2 px each): 88 x 15 min = 22 h.
constexpr uint32_t HISTORY_INTERVAL_MIN = 15;
constexpr int      HISTORY_SIZE         = 88;

// ============================================================
//                          BATTERY
// ============================================================
constexpr float BATTERY_DIVIDER_RATIO = 3.0f;   // V_battery = V_pin * ratio
constexpr int   BATTERY_ADC_SAMPLES   = 16;     // Averaged per reading
constexpr float BATTERY_SMOOTHING     = 0.1f;   // EMA weight of each new reading
constexpr float BATTERY_ABSENT_BELOW_V = 2.5f;  // Below this: USB power only
constexpr float BATTERY_FULL_ABOVE_V   = 4.15f; // Above this: charging / charged

// ============================================================
//                          LOW POWER
// ============================================================
// When enabled:
//   - The screen refreshes once per minute and seconds are hidden
//   - The CPU light-sleeps between refreshes (the reflective LCD keeps the image)
//   - WiFi is only switched on to re-sync the clock (see NTP_RESYNC_MIN)
// Note: USB serial output is interrupted while the board sleeps.
constexpr bool LOW_POWER_MODE = false;

// Wake up slightly after the minute rolls over, so the RTC already shows it.
constexpr uint32_t LOW_POWER_WAKE_MARGIN_MS = 200;

// ============================================================
//                          MAIN LOOP
// ============================================================
constexpr uint32_t LOOP_DELAY_MS = 20;
