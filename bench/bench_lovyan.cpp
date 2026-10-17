#include "prelude.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "bench.h"

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 10000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 18;
      cfg.pin_mosi = 23;
      cfg.pin_miso = 19;
      cfg.pin_dc = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = 4;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.readable = true;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

const char *LIB_NAME = "LovyanGFX";
LGFX tft;
LGFX_Sprite spr(&tft);
int sprBits = 0;

void benchSync() { tft.waitDMA(); }
void benchPushStrip(int16_t y, uint16_t *buf) { tft.pushImage(0, y, 320, 40, (lgfx::rgb565_t *)buf); }
bool animBegin(String &info) {
  if (!sprBits) {
    spr.setColorDepth(16);
    if (spr.createSprite(320, 240)) sprBits = 16;
    else {
      spr.setColorDepth(8);
      sprBits = spr.createSprite(320, 240) ? 8 : -1;
    }
  }
  if (sprBits < 0) {
    info = "Sprite_320x240:kein_RAM";
    return false;
  }
  info = String("Sprite_") + sprBits + "bit";
  return true;
}
void animDraw(int f, bool full) { drawScene(spr, f, full); }
void animPush() { spr.pushSprite(0, 0); }
void benchExtra() {}

void setup() {
  Serial.begin(921600);
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
  tft.init();
  tft.setRotation(1);
  delay(300);
  runAll(tft);
}
void loop() {}
