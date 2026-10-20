#include "speaker.h"

#include <ESP_I2S.h>
#include <math.h>

namespace {

constexpr uint8_t REG_CHIP_ID1 = 0xFD;
constexpr uint8_t CHIP_ID1_ES8311 = 0x83;

// Register sequence ported from Espressif's esp_codec_dev ES8311 driver
// (open + set_fs + start), as used by the Waveshare audio example:
// I2S slave, MCLK from the pin at 256 x fs, 16-bit Philips I2S, DAC only.
struct RegValue {
  uint8_t reg, value;
};
constexpr RegValue CODEC_INIT[] = {
  {0x44, 0x08}, {0x44, 0x08},  // I2C noise immunity (written twice: first write may fail)
  {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
  {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
  {0x00, 0x80},  // Power on, slave mode
  {0x01, 0x3F},  // Clocks on, MCLK from pin, not inverted
  {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A},
  {0x44, 0x08},  // No DAC-to-ADC reference loop (microphone unused)
  // Clock dividers for MCLK = 4.096 MHz, fs = 16 kHz
  {0x02, 0x00}, {0x05, 0x00}, {0x03, 0x10}, {0x04, 0x20}, {0x07, 0x00}, {0x08, 0xFF},
  {0x06, 0x03},  // BCLK divider, SCLK not inverted
  // Serial ports: DAC in 16-bit I2S, ADC 16-bit and muted
  {0x09, 0x0C}, {0x0A, 0x4C},
  // Start
  {0x17, 0xBF}, {0x0E, 0x02}, {0x12, 0x00}, {0x14, 0x1A}, {0x0D, 0x01},
  {0x15, 0x40}, {0x37, 0x08}, {0x45, 0x00},
  {0x32, 0xBF},  // DAC volume 0 dB (loudness is set by the sample amplitude)
  {0x31, 0x00},  // DAC unmuted
};

struct PatternSpec {
  uint8_t count;
  uint16_t onMs, offMs;
};

PatternSpec patternSpec(BeepPattern p)
{
  switch (p) {
    case BeepPattern::Early: return {2, 150, 150};
    case BeepPattern::Start: return {3, 400, 200};
    case BeepPattern::Test:
    default:                 return {1, 80, 0};
  }
}

constexpr int CHUNK_FRAMES = 128;
constexpr uint32_t FADE_MS = 4;          // Envelope to avoid clicks
constexpr uint32_t DMA_DRAIN_MS = 120;   // Silence so the DMA queue plays out before the PA turns off

I2SClass i2s;

}  // namespace

Speaker::Speaker(TwoWire &wire, uint8_t address, const SpeakerPins &pins)
  : _wire(wire), _address(address), _pins(pins) {}

bool Speaker::writeReg(uint8_t reg, uint8_t value)
{
  _wire.beginTransmission(_address);
  _wire.write(reg);
  _wire.write(value);
  return _wire.endTransmission() == 0;
}

bool Speaker::readReg(uint8_t reg, uint8_t &value)
{
  _wire.beginTransmission(_address);
  _wire.write(reg);
  if (_wire.endTransmission(false) != 0) return false;
  if (_wire.requestFrom(_address, static_cast<uint8_t>(1)) != 1) return false;
  value = _wire.read();
  return true;
}

bool Speaker::initCodec()
{
  bool ok = true;
  for (const RegValue &rv : CODEC_INIT) {
    // The very first write is allowed to fail (see above)
    if (!writeReg(rv.reg, rv.value) && &rv != &CODEC_INIT[0]) ok = false;
  }
  return ok;
}

bool Speaker::begin(uint32_t sampleRate, uint32_t toneHz, int volumePct)
{
  _sampleRate = sampleRate;
  _toneHz = toneHz;
  _amplitude = static_cast<int16_t>(32767L * constrain(volumePct, 0, 100) / 100);

  pinMode(_pins.pa, OUTPUT);
  digitalWrite(_pins.pa, LOW);

  uint8_t id = 0;
  if (!readReg(REG_CHIP_ID1, id) || id != CHIP_ID1_ES8311) return false;

  // I2S (and its MCLK) and the codec are only brought up while a pattern plays
  if (xTaskCreate(taskMain, "speaker", 4096, this, 2, &_task) != pdPASS) return false;
  _present = true;
  return true;
}

void Speaker::beep(BeepPattern pattern)
{
  if (!_present) return;
  _playing = true;  // Already busy for the caller, before the task wakes up (it may go to sleep)
  _stopRequested = true;  // Ends the current pattern; the task then picks up the new one
  xTaskNotify(_task, static_cast<uint32_t>(pattern), eSetValueWithOverwrite);
}

void Speaker::stop()
{
  _stopRequested = true;
}

void Speaker::writeTone(uint32_t ms)
{
  int16_t buf[CHUNK_FRAMES * 2];
  const uint32_t total = _sampleRate * ms / 1000;
  const uint32_t fade = _sampleRate * FADE_MS / 1000;
  const float step = 2.0f * PI * _toneHz / _sampleRate;

  for (uint32_t n = 0; n < total && !_stopRequested;) {
    int frames = min<uint32_t>(CHUNK_FRAMES, total - n);
    for (int i = 0; i < frames; i++, n++) {
      float env = 1.0f;
      if (n < fade)              env = static_cast<float>(n) / fade;
      else if (total - n < fade) env = static_cast<float>(total - n) / fade;
      int16_t s = static_cast<int16_t>(_amplitude * env * sinf(step * n));
      buf[2 * i] = buf[2 * i + 1] = s;
    }
    i2s.write(reinterpret_cast<const uint8_t *>(buf), frames * 2 * sizeof(int16_t));
  }
}

void Speaker::writeSilence(uint32_t ms)
{
  int16_t buf[CHUNK_FRAMES * 2] = {};
  const uint32_t total = _sampleRate * ms / 1000;
  for (uint32_t n = 0; n < total; n += CHUNK_FRAMES) {
    int frames = min<uint32_t>(CHUNK_FRAMES, total - n);
    i2s.write(reinterpret_cast<const uint8_t *>(buf), frames * 2 * sizeof(int16_t));
  }
}

void Speaker::play(BeepPattern pattern)
{
  const PatternSpec spec = patternSpec(pattern);

  _playing = true;
  // MCLK must be running before the codec is configured
  i2s.setPins(_pins.bclk, _pins.ws, _pins.dout, -1, _pins.mclk);
  if (!i2s.begin(I2S_MODE_STD, _sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO) || !initCodec()) {
    Serial.println("[audio] Could not start I2S/codec");
    i2s.end();
    _playing = false;
    return;
  }
  digitalWrite(_pins.pa, HIGH);
  writeSilence(20);  // Let the amplifier settle
  for (int i = 0; i < spec.count && !_stopRequested; i++) {
    writeTone(spec.onMs);
    if (i < spec.count - 1 && !_stopRequested) writeSilence(spec.offMs);
  }
  writeSilence(DMA_DRAIN_MS);
  digitalWrite(_pins.pa, LOW);
  writeReg(0x00, 0x00);  // Codec off (reset/power-down) before MCLK stops
  i2s.end();
  _playing = false;
}

void Speaker::taskMain(void *arg)
{
  Speaker *self = static_cast<Speaker *>(arg);
  for (;;) {
    uint32_t pattern = 0;
    xTaskNotifyWait(0, UINT32_MAX, &pattern, portMAX_DELAY);
    self->_stopRequested = false;
    self->play(static_cast<BeepPattern>(pattern));
  }
}
