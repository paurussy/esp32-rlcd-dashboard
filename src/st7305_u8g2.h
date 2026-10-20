/*
  st7305_u8g2.h - U8g2 adapter for the ST7305 reflective LCD (300x400, 1 bpp)
  ---------------------------------------------------------------------------
  Exposes the panel as a regular U8G2 object. U8g2 renders into a RAM buffer
  and the custom display callback repacks each tile row into the ST7305's
  native pixel layout before sending it over SPI.
*/
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

class ST7305_U8g2 {
public:
  ST7305_U8g2(int sck, int mosi, int dc, int cs, int rst);
  ~ST7305_U8g2();

  ST7305_U8g2(const ST7305_U8g2 &) = delete;
  ST7305_U8g2 &operator=(const ST7305_U8g2 &) = delete;

  // tileBufHeight = 0 selects full-buffer mode (~15 KB of RAM for 300x400).
  // A smaller value works with firstPage()/nextPage() paging.
  // Returns false if the frame buffer could not be allocated.
  bool begin(uint8_t tileBufHeight = 0, const u8g2_cb_t *rotation = U8G2_R0);

  U8G2 &u8g2() { return _u8g2; }

private:
  int _sck;
  int _mosi;
  int _dc;
  int _cs;
  int _rst;
  SPIClass *_spi = nullptr;
  U8G2 _u8g2;
  uint8_t *_buffer = nullptr;

  void hardwareReset();
  void initPanel();

  void writeCommand(uint8_t cmd);
  void writeCommandData(uint8_t cmd, const uint8_t *data, size_t len);
  void drawTileRow(const u8x8_tile_t *tile);

  static uint8_t displayCallback(u8x8_t *u8x8, uint8_t msg, uint8_t argInt, void *argPtr);
  static uint8_t byteCallback(u8x8_t *u8x8, uint8_t msg, uint8_t argInt, void *argPtr);
};
