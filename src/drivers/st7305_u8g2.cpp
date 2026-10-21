#include "st7305_u8g2.h"

#include <driver/gpio.h>

namespace {

constexpr uint32_t SPI_CLOCK_HZ = 24000000;

constexpr int PANEL_WIDTH  = 300;
constexpr int PANEL_HEIGHT = 400;

// U8g2 tiles are 8x8 px: 38 x 8 = 304 columns (>= 300), 50 x 8 = 400 rows.
constexpr uint8_t TILE_WIDTH  = 38;
constexpr uint8_t TILE_HEIGHT = 50;
constexpr size_t  BUFFER_ROW_BYTES = TILE_WIDTH * 8;

// The ST7305 addresses columns in groups of 12 px, starting at 0x12.
// Column addresses run right-to-left, mirrored around 0x3C.
constexpr int COLUMN_ADDR_BASE   = 0x12;
constexpr int COLUMN_ADDR_MIRROR = 0x3C;
constexpr int PX_PER_COLUMN_ADDR = 12;

// ST7305 commands
constexpr uint8_t CMD_SLEEP_OUT        = 0x11;
constexpr uint8_t CMD_INVERSION_ON     = 0x21;
constexpr uint8_t CMD_DISPLAY_ON       = 0x29;
constexpr uint8_t CMD_COLUMN_ADDR_SET  = 0x2A;
constexpr uint8_t CMD_ROW_ADDR_SET     = 0x2B;
constexpr uint8_t CMD_MEMORY_WRITE     = 0x2C;
constexpr uint8_t CMD_HIGH_POWER_MODE  = 0x38;  // Panel refresh at the full frame rate: while a frame is written
constexpr uint8_t CMD_LOW_POWER_MODE   = 0x39;  // Slow refresh: the rest of the time

// Only one panel per board, so a single instance is enough for the C callbacks.
ST7305_U8g2 *g_instance = nullptr;

u8x8_display_info_t g_displayInfo = {
  /* chip_enable_level = */ 0,
  /* chip_disable_level = */ 1,
  /* post_chip_enable_wait_ns = */ 0,
  /* pre_chip_disable_wait_ns = */ 0,
  /* reset_pulse_width_ms = */ 20,
  /* post_reset_wait_ms = */ 50,
  /* sda_setup_time_ns = */ 0,
  /* sck_pulse_width_ns = */ 0,
  /* sck_clock_hz = */ SPI_CLOCK_HZ,
  /* spi_mode = */ 0,
  /* i2c_bus_clock_100kHz = */ 4,
  /* data_setup_time_ns = */ 0,
  /* write_pulse_width_ns = */ 0,
  /* tile_width = */ TILE_WIDTH,
  /* tile_height = */ TILE_HEIGHT,
  /* default_x_offset = */ 0,
  /* flip_mode_x_offset = */ 0,
  /* pixel_width = */ PANEL_WIDTH,
  /* pixel_height = */ PANEL_HEIGHT
};

}  // namespace

ST7305_U8g2::ST7305_U8g2(int sck, int mosi, int dc, int cs, int rst)
  : _sck(sck), _mosi(mosi), _dc(dc), _cs(cs), _rst(rst)
{
  _spi = new SPIClass(HSPI);
  g_instance = this;
}

ST7305_U8g2::~ST7305_U8g2()
{
  if (_spi) {
    _spi->end();
    delete _spi;
  }
  free(_buffer);
  if (g_instance == this) {
    g_instance = nullptr;
  }
}

bool ST7305_U8g2::begin(uint8_t tileBufHeight, const u8g2_cb_t *rotation)
{
  pinMode(_dc, OUTPUT);
  pinMode(_cs, OUTPUT);
  pinMode(_rst, OUTPUT);
  digitalWrite(_cs, HIGH);
  digitalWrite(_dc, HIGH);
  digitalWrite(_rst, HIGH);

  _spi->begin(_sck, -1, _mosi, -1);
  _spi->beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));

  u8g2_t *u = _u8g2.getU8g2();
  u8x8_Setup(u8g2_GetU8x8(u), displayCallback, u8x8_dummy_cb, byteCallback, u8x8_dummy_cb);

  uint8_t rows = (tileBufHeight == 0) ? TILE_HEIGHT : min(tileBufHeight, TILE_HEIGHT);
  size_t size = BUFFER_ROW_BYTES * rows;
  _buffer = static_cast<uint8_t *>(calloc(size, 1));
  if (!_buffer) {
    return false;
  }

  u8g2_SetupBuffer(u, _buffer, rows, u8g2_ll_hvline_vertical_top_lsb, rotation);
  u8g2_InitDisplay(u);
  u8g2_SetPowerSave(u, 0);
  return true;
}

void ST7305_U8g2::holdPins(bool hold)
{
  for (int pin : {_cs, _dc, _rst}) {
    gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    if (hold) gpio_hold_en(gpio);
    else      gpio_hold_dis(gpio);
  }
}

void ST7305_U8g2::hardwareReset()
{
  digitalWrite(_rst, HIGH);
  delay(50);
  digitalWrite(_rst, LOW);
  delay(20);
  digitalWrite(_rst, HIGH);
  delay(50);
}

void ST7305_U8g2::writeCommand(uint8_t cmd)
{
  writeCommandData(cmd, nullptr, 0);
}

void ST7305_U8g2::writeCommandData(uint8_t cmd, const uint8_t *data, size_t len)
{
  digitalWrite(_dc, LOW);
  digitalWrite(_cs, LOW);
  _spi->transfer(cmd);
  if (len > 0) {
    digitalWrite(_dc, HIGH);
    _spi->transferBytes(const_cast<uint8_t *>(data), nullptr, len);
  }
  digitalWrite(_cs, HIGH);
}

// Sends one tile row (8 px high) to the panel. U8g2 stores pixels as vertical
// bytes (LSB on top); the ST7305 expects each byte to hold a 2x4 px block, so
// every 4 source columns x 2 bits are interleaved into one output byte.
void ST7305_U8g2::drawTileRow(const u8x8_tile_t *tile)
{
  int firstCol = tile->x_pos * 8;
  int lastCol = min((tile->x_pos + tile->cnt) * 8 - 1, PANEL_WIDTH - 1);

  int addrStart = COLUMN_ADDR_BASE + firstCol / PX_PER_COLUMN_ADDR;
  int addrEnd = COLUMN_ADDR_BASE + lastCol / PX_PER_COLUMN_ADDR;
  int sendStart = (addrStart - COLUMN_ADDR_BASE) * 3;
  int sendCount = (addrEnd - addrStart + 1) * 3;

  int addrFirstCol = (addrStart - COLUMN_ADDR_BASE) * PX_PER_COLUMN_ADDR;
  int addrLastCol = min((addrEnd - COLUMN_ADDR_BASE) * PX_PER_COLUMN_ADDR + PX_PER_COLUMN_ADDR - 1,
                        PANEL_WIDTH - 1);

  const uint8_t *rowBase = tile->tile_ptr - static_cast<uint16_t>(tile->x_pos) * 8U;

  // sendBuffer() writes the tile rows top to bottom: full power for the whole frame
  if (tile->y_pos == 0) writeCommand(CMD_HIGH_POWER_MODE);

  const uint8_t colBounds[] = {static_cast<uint8_t>(COLUMN_ADDR_MIRROR - addrEnd),
                               static_cast<uint8_t>(COLUMN_ADDR_MIRROR - addrStart)};
  writeCommandData(CMD_COLUMN_ADDR_SET, colBounds, sizeof(colBounds));

  const uint8_t rowBounds[] = {static_cast<uint8_t>(tile->y_pos * 4),
                               static_cast<uint8_t>(tile->y_pos * 4 + 3)};
  writeCommandData(CMD_ROW_ADDR_SET, rowBounds, sizeof(rowBounds));

  // LUT[column within group][2-bit pixel pair] -> bits in the output byte
  static const uint8_t LUT[4][4] = {
    {0x00, 0x80, 0x40, 0xC0},
    {0x00, 0x20, 0x10, 0x30},
    {0x00, 0x08, 0x04, 0x0C},
    {0x00, 0x02, 0x01, 0x03},
  };

  uint8_t out[PANEL_WIDTH] = {0};
  for (int subRow = 0; subRow < 4; subRow++) {
    int shift = subRow * 2;
    int idx = subRow * sendCount + (addrFirstCol >> 2) - sendStart;

    for (int col = addrFirstCol; col <= addrLastCol; col += 4, idx++) {
      out[idx] = LUT[0][(rowBase[col] >> shift) & 3]
               | LUT[1][(rowBase[col + 1] >> shift) & 3]
               | LUT[2][(rowBase[col + 2] >> shift) & 3]
               | LUT[3][(rowBase[col + 3] >> shift) & 3];
    }
  }

  writeCommandData(CMD_MEMORY_WRITE, out, static_cast<size_t>(sendCount) * 4U);

  if (tile->y_pos == TILE_HEIGHT - 1) writeCommand(CMD_LOW_POWER_MODE);
}

// Communication is handled directly in this class, so U8g2's byte layer is a no-op.
uint8_t ST7305_U8g2::byteCallback(u8x8_t *, uint8_t, uint8_t, void *)
{
  return 1;
}

uint8_t ST7305_U8g2::displayCallback(u8x8_t *u8x8, uint8_t msg, uint8_t, void *argPtr)
{
  if (!g_instance) {
    return 0;
  }

  switch (msg) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
      u8x8_d_helper_display_setup_memory(u8x8, &g_displayInfo);
      return 1;

    case U8X8_MSG_DISPLAY_INIT:
      g_instance->initPanel();
      return 1;

    case U8X8_MSG_DISPLAY_DRAW_TILE:
      g_instance->drawTileRow(static_cast<const u8x8_tile_t *>(argPtr));
      return 1;

    default:
      return 0;
  }
}

// Power-up sequence from the Waveshare reference driver.
void ST7305_U8g2::initPanel()
{
  hardwareReset();

  const uint8_t nvmLoad[]        = {0x17, 0x02};
  const uint8_t boosterEnable[]  = {0x01};
  const uint8_t gateVoltage[]    = {0x11, 0x04};
  const uint8_t vshp[]           = {0x69, 0x69, 0x69, 0x69};
  const uint8_t vslp[]           = {0x19, 0x19, 0x19, 0x19};
  const uint8_t vshn[]           = {0x4B, 0x4B, 0x4B, 0x4B};
  const uint8_t oscillator[]     = {0x80, 0xE9};
  const uint8_t frameRate[]      = {0x02};
  const uint8_t gateEqHighPwr[]  = {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  const uint8_t gateEqLowPwr[]   = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  const uint8_t gateTiming[]     = {0x32, 0x03, 0x1F};
  const uint8_t sourceEq[]       = {0x13};
  const uint8_t gateLines[]      = {0x64};  // 100 x 4 = 400 lines
  const uint8_t sourceVoltage[]  = {0x00};
  const uint8_t memoryAccess[]   = {0x48};
  const uint8_t dataFormat[]     = {0x11};
  const uint8_t gammaMode[]      = {0x20};
  const uint8_t panelSetting[]   = {0x29};
  const uint8_t columnWindow[]   = {0x12, 0x2A};
  const uint8_t rowWindow[]      = {0x00, 0xC7};
  const uint8_t tearingEffect[]  = {0x00};
  const uint8_t autoPowerDown[]  = {0xFF};

  writeCommandData(0xD6, nvmLoad, sizeof(nvmLoad));
  writeCommandData(0xD1, boosterEnable, sizeof(boosterEnable));
  writeCommandData(0xC0, gateVoltage, sizeof(gateVoltage));
  writeCommandData(0xC1, vshp, sizeof(vshp));
  writeCommandData(0xC2, vslp, sizeof(vslp));
  writeCommandData(0xC4, vshn, sizeof(vshn));
  writeCommandData(0xC5, vslp, sizeof(vslp));  // VSLN uses the same values as VSLP
  writeCommandData(0xD8, oscillator, sizeof(oscillator));
  writeCommandData(0xB2, frameRate, sizeof(frameRate));
  writeCommandData(0xB3, gateEqHighPwr, sizeof(gateEqHighPwr));
  writeCommandData(0xB4, gateEqLowPwr, sizeof(gateEqLowPwr));
  writeCommandData(0x62, gateTiming, sizeof(gateTiming));
  writeCommandData(0xB7, sourceEq, sizeof(sourceEq));
  writeCommandData(0xB0, gateLines, sizeof(gateLines));
  writeCommand(CMD_SLEEP_OUT);
  delay(120);
  writeCommandData(0xC9, sourceVoltage, sizeof(sourceVoltage));
  writeCommandData(0x36, memoryAccess, sizeof(memoryAccess));
  writeCommandData(0x3A, dataFormat, sizeof(dataFormat));
  writeCommandData(0xB9, gammaMode, sizeof(gammaMode));
  writeCommandData(0xB8, panelSetting, sizeof(panelSetting));
  writeCommand(CMD_INVERSION_ON);
  writeCommandData(CMD_COLUMN_ADDR_SET, columnWindow, sizeof(columnWindow));
  writeCommandData(CMD_ROW_ADDR_SET, rowWindow, sizeof(rowWindow));
  writeCommandData(0x35, tearingEffect, sizeof(tearingEffect));
  writeCommandData(0xD0, autoPowerDown, sizeof(autoPowerDown));
  writeCommand(CMD_LOW_POWER_MODE);  // Each frame switches to high power while it is written
  writeCommand(CMD_DISPLAY_ON);
}
