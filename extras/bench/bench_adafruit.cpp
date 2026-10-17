#include "prelude.h"
#include <SPI.h>
#include <Adafruit_ILI9341.h>
#include "bench.h"

const char *LIB_NAME = "Adafruit_ILI9341";
Adafruit_ILI9341 tft(15, 2, 4);
GFXcanvas16 *canvas = nullptr;

void benchSync() {}
void benchPushStrip(int16_t y, uint16_t *buf) { tft.drawRGBBitmap(0, y, buf, 320, 40); }
bool animBegin(String &info) {
  if (!canvas) canvas = new GFXcanvas16(320, 240);
  if (!canvas->getBuffer()) {
    info = "GFXcanvas16_320x240:kein_RAM";
    return false;
  }
  info = "GFXcanvas16_16bit";
  return true;
}
void animDraw(int f, bool full) { drawScene(*canvas, f, full); }
void animPush() { tft.drawRGBBitmap(0, 0, canvas->getBuffer(), 320, 240); }
void benchExtra() {}

void setup() {
  Serial.begin(921600);
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
  tft.begin(40000000);
  tft.setRotation(1);
  delay(300);
  runAll(tft);
}
void loop() {}
