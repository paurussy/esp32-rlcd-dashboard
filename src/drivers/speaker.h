/*
  speaker.h - Beeps through the on-board ES8311 codec and speaker amplifier
  -------------------------------------------------------------------------
  The ES8311 is configured over I2C as an I2S slave (16-bit stereo, MCLK =
  256 x fs); the ESP32 generates a sine tone. Patterns play in a background
  task, so beep() returns immediately. I2S, the codec and the amplifier are only
  powered while a pattern plays.
*/
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include <atomic>

enum class BeepPattern : uint8_t {
  Test = 1,   // One short beep
  Early,      // Two short beeps: meeting starts soon
  Start,      // Three long beeps: meeting starts now
};

struct SpeakerPins {
  int mclk, bclk, ws, dout, pa;
};

class Speaker {
public:
  Speaker(TwoWire &wire, uint8_t address, const SpeakerPins &pins);

  // Detects the codec and starts the playback task.
  bool begin(uint32_t sampleRate, uint32_t toneHz, int volumePct);

  // Amplifier off and codec powered down, without starting the playback task: for
  // when the beeps are not used.
  void shutdown();

  // Starts a pattern, replacing the one playing (if any).
  void beep(BeepPattern pattern);

  // Cuts the current pattern short.
  void stop();

  bool isPlaying() const { return _playing; }
  bool isPresent() const { return _present; }

private:
  TwoWire &_wire;
  uint8_t _address;
  SpeakerPins _pins;
  uint32_t _sampleRate = 16000;
  uint32_t _toneHz = 1000;
  int16_t _amplitude = 0;
  bool _present = false;
  std::atomic<bool> _playing{false};         // Shared with the playback task
  std::atomic<bool> _stopRequested{false};
  TaskHandle_t _task = nullptr;

  bool writeReg(uint8_t reg, uint8_t value);
  bool readReg(uint8_t reg, uint8_t &value);
  bool initCodec();
  void powerDown();
  void play(BeepPattern pattern);
  void writeTone(uint32_t ms);
  void writeSilence(uint32_t ms);
  static void taskMain(void *arg);
};
