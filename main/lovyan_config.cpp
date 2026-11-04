#include "include/lovyan_config.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "lvgl.h"

// the display setup.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 60000000; // if u read dis, and somtin dont work, change it to 40mhz
      cfg.freq_read  = 5000000; 
      cfg.pin_sclk   = 12;  // SCLK pin
      cfg.pin_mosi = 11; // MOSI pin
      cfg.pin_miso = 13; // MISO
      cfg.pin_dc = 9;    // dc pin
      cfg.dma_channel = SPI_DMA_CH_AUTO; // DMA

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 10;  // cs pin
      cfg.pin_rst = 14; // rst pin

      //screen resolution
      cfg.panel_width = 320;
      cfg.panel_height = 480;

      cfg.rgb_order = false;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = false;

      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance);
  }
};

static LGFX lcd;

extern "C" {
  ili9488_t get_ili9488() {
    return (ili9488_t)&lcd;
  }

//helper
  static void toilet_lv(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int32_t x = area->x1;
    int32_t y = area->y1;
    int32_t w = (area->x2 - area->x1) + 1;
    int32_t h = (area->y2 - area->y1) + 1;

    lcd.waitDMA();
    lcd.pushImageDMA(x, y, w, h, (uint16_t *)px_map);

    lv_display_flush_ready(disp);
}

// transform lovyanGFX to LVGL
  void GF_to_LV() {
      lcd.init();
      lcd.setRotation(1);
      /* swap the bytes since the one I use is ILI9488 which is big-endian
         and the lvgl is little-endian. anyway it render normaly this way XD */
      lcd.setSwapBytes(1);
      lv_display_t *disp = lv_display_create(480, 320);
      
      alignas(4) static uint8_t sigma1[480 * 40 * sizeof(uint16_t)];
      alignas(4) static uint8_t sigma2[480 * 40 * sizeof(uint16_t)];
      lv_display_set_buffers(disp,
                             sigma1,
                             sigma2, 
                             sizeof(sigma1), 
                             LV_DISPLAY_RENDER_MODE_PARTIAL
                            );

      lv_display_set_flush_cb(disp, toilet_lv);
  }
}
