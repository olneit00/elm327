#pragma once
//
// User display header consumed by the LVGL built-in LovyanGFX driver.
//
// The file name is referenced from lv_conf.h via LV_LGFX_USER_INCLUDE.
// LVGL's lv_lovyan_gfx.cpp expects this header to declare a class literally
// named `LGFX` (which derives from lgfx::LGFX_Device) and instantiates it
// internally with `new LGFX()`. This header therefore only defines the class,
// never a global instance, so LVGL keeps sole ownership of the panel.
//
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "hardware/Pins.h"

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

 public:
  LGFX(void) {
    {  // SPI bus
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;  // ESP32 default VSPI
      cfg.spi_mode = 0;
      cfg.freq_write = 26000000;  // GC9A01-stable; louder than 40 MHz on cheap modules
      cfg.freq_read = 16000000;
      // Module has a dedicated DC line => standard 4-wire SPI, not 3-wire.
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = pins::SCL_PIN;
      cfg.pin_mosi = pins::SDA_PIN;
      cfg.pin_miso = -1;
      cfg.pin_dc = pins::DC_PIN;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {  // GC9A01 panel
      auto cfg = _panel_instance.config();
      cfg.pin_cs = pins::CS_PIN;
      cfg.pin_rst = pins::RST_PIN;
      cfg.pin_busy = -1;

      cfg.memory_width = pins::SCREEN_WIDTH;
      cfg.memory_height = pins::SCREEN_HEIGHT;
      cfg.panel_width = pins::SCREEN_WIDTH;
      cfg.panel_height = pins::SCREEN_HEIGHT;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;

      cfg.dummy_read_pixel = 16;
      cfg.dummy_read_bits = 0;
      cfg.readable = false;
      cfg.invert = pins::INVERT;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance);
  }
};