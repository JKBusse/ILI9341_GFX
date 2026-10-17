#include "prelude.h"
#include <TFT_eSPI.h>
#include "bench.h"

const char *LIB_NAME = "TFT_eSPI";
TFT_eSPI tft;
TFT_eSprite spr(&tft);
int sprBits = 0;

void benchSync() {}
void benchPushStrip(int16_t y, uint16_t *buf) { tft.pushImage(0, y, 320, 40, buf); }
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
