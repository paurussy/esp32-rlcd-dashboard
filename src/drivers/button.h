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
  bool idle() const { return digitalRead(pin) == HIGH && stableLevel == HIGH && !clickPending; }

  // Input with pull-up that also wakes the chip from light sleep.
  void begin() const
  {
    pinMode(pin, INPUT_PULLUP);
    gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }

  const int pin;

private:
  enum class Edge { None, Down, Up };

  Edge debounce()
  {
    const bool level = digitalRead(pin);
    if (level != lastLevel) {
      lastLevel = level;
      changedMs = millis();
    }
    if (level == stableLevel || millis() - changedMs < BUTTON_DEBOUNCE_MS) return Edge::None;

    stableLevel = level;
    return level == LOW ? Edge::Down : Edge::Up;
  }

  bool stableLevel = HIGH;
  bool lastLevel = HIGH;
  uint32_t changedMs = 0;

  bool held = false;          // Down, for the long press
  bool consumed = false;      // This press already fired (long or double): its release is silent
  bool clickPending = false;  // Released once, waiting to see if a second click follows
  uint32_t downMs = 0;
  uint32_t clickMs = 0;
};
