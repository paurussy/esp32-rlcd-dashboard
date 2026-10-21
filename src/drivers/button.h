/*
  button.h - Active-low push button with debounce, clicks and long presses
  ------------------------------------------------------------------------
  Header-only. begin() also arms the pin as a light-sleep wake-up source.
*/
#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "../../config.h"

enum class Gesture { None, Click, DoubleClick, LongPress };

// Active-low button with debounce. Use either pressed() or poll() on a given button.
struct Button {
  explicit Button(int pin) : pin(pin) {}

  // Debounced press, at the moment the button goes down.
  bool pressed() { return debounce() == Edge::Down; }

  // Click, double click and long press. A click fires on release, and with
  // `allowDouble` only once it is known that no second click follows. A long
  // press fires while the button is still held.
  Gesture poll(bool allowDouble)
  {
    const uint32_t now = millis();
    switch (debounce()) {
      case Edge::Down:
        downMs = now;
        held = true;
        if (allowDouble && clickPending && now - clickMs < BUTTON_DOUBLE_CLICK_MS) {
          clickPending = false;
          consumed = true;  // The release that follows is not a click
          return Gesture::DoubleClick;
        }
        consumed = false;
        break;
      case Edge::Up:
        held = false;
        if (consumed) break;
        if (!allowDouble) return Gesture::Click;
        clickPending = true;
        clickMs = now;
        break;
      case Edge::None:
        break;
    }

    if (held && !consumed && now - downMs >= BUTTON_LONG_PRESS_MS) {
      consumed = true;
      clickPending = false;
      return Gesture::LongPress;
    }
    if (clickPending && (!allowDouble || now - clickMs >= BUTTON_DOUBLE_CLICK_MS)) {
      clickPending = false;
      return Gesture::Click;
    }
    return Gesture::None;
  }

  // Released, settled and with no click waiting for a second one: safe to sleep
  // (the wake-up on a low level would fire at once otherwise).
  bool idle() const { return digitalRead(pin) == HIGH && stableLevel == HIGH && !clickPending && !latched; }

  // Input with pull-up. A falling-edge interrupt latches every press, so one that
  // starts and ends while the loop is blocked is still seen afterwards.
  void begin()
  {
    pinMode(pin, INPUT_PULLUP);
    attachInterruptArg(pin, onFall, this, FALLING);
    esp_sleep_enable_gpio_wakeup();
  }

  // Around light sleep: the pin wakes the chip on a low level (which replaces the
  // edge interrupt while it is armed). The CPU interrupt stays off meanwhile: a
  // level interrupt on a button still held would fire non-stop on wake-up and
  // starve the loop (interrupt watchdog reset, and with BOOT down, download mode).
  void armWake() const
  {
    gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL);
    gpio_intr_disable(static_cast<gpio_num_t>(pin));
  }

  void afterWake()
  {
    gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
    gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_NEGEDGE);
    gpio_intr_enable(static_cast<gpio_num_t>(pin));
    if (digitalRead(pin) == LOW) latched = true;  // This press woke the chip, even if it is short
  }

  const int pin;

private:
  enum class Edge { None, Down, Up };

  // Bounce needs no filtering here: edges seen while a press is held or released are
  // cleared when its release is confirmed (see debounce)
  static void IRAM_ATTR onFall(void *arg) { static_cast<Button *>(arg)->latched = true; }

  Edge debounce()
  {
    const bool level = digitalRead(pin);
    if (latched && stableLevel == HIGH) {
      // A press the polling missed (or has not confirmed yet): count it now
      latched = false;
      lastLevel = level;
      changedMs = millis();
      stableLevel = LOW;
      return Edge::Down;
    }
    if (level != lastLevel) {
      lastLevel = level;
      changedMs = millis();
    }
    if (level == stableLevel || millis() - changedMs < BUTTON_DEBOUNCE_MS) return Edge::None;

    stableLevel = level;
    if (level == HIGH) latched = false;  // Edges seen while releasing are contact bounce
    return level == LOW ? Edge::Down : Edge::Up;
  }

  volatile bool latched = false;  // Falling edge seen by the interrupt, not consumed yet

  bool stableLevel = HIGH;
  bool lastLevel = HIGH;
  uint32_t changedMs = 0;

  bool held = false;          // Down, for the long press
  bool consumed = false;      // This press already fired (long or double): its release is silent
  bool clickPending = false;  // Released once, waiting to see if a second click follows
  uint32_t downMs = 0;
  uint32_t clickMs = 0;
};
