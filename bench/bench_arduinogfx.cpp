#include "prelude.h"
#include <Arduino_GFX_Library.h>
#include "bench.h"

const char *LIB_NAME = "Arduino_GFX";
Arduino_DataBus *bus = new Arduino_ESP32SPI(2 /*DC*/, 15 /*CS*/, 18, 23, 19, VSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, 4 /*RST*/, 1 /*Rotation*/);
Arduino_Canvas *canvas = nullptr;
int canvasOk = 0;

void benchSync() {}
void benchPushStrip(int16_t y, uint16_t *buf) { gfx->draw16bitRGBBitmap(0, y, buf, 320, 40); }
bool animBegin(String &info) {
  if (!canvasOk) {
    canvas = new Arduino_Canvas(320, 240, gfx);
    canvasOk = canvas->begin(GFX_SKIP_OUTPUT_BEGIN) ? 1 : -1;
  }
  if (canvasOk < 0) {
    info = "Canvas_320x240:kein_RAM";
    return false;
  }
  info = "Canvas_16bit";
  return true;
}
void animDraw(int f, bool full) { drawScene(*canvas, f, full); }
void animPush() { canvas->flush(); }
void benchExtra() {}

void setup() {
  Serial.begin(921600);
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
  gfx->begin(40000000);
  delay(300);
  runAll(*gfx);
}
void loop() {}
