#include "prelude.h"
#include "ILI9341_GFX.h"
#include "ILI9341_Framebuffer.h"
#include "bench.h"

const char *LIB_NAME = "ILI9341_GFX";
ILI9341_GFX tft(15, 2, 4);
ILI9341_Framebuffer *fb = nullptr;

void benchSync() { tft.finishWrites(); }
void benchPushStrip(int16_t y, uint16_t *buf) { tft.pushImage(0, y, 320, 40, buf); }
bool animBegin(String &info) {
  if (!fb) {
    fb = new ILI9341_Framebuffer(tft);
    if (!fb->begin()) {
      info = "Framebuffer:kein_RAM";
      delete fb;
      fb = nullptr;
      return false;
    }
    fb->setRotation(1);
  }
  info = fb->vsync() ? "Framebuffer_16bit_Diff_VSync" : "Framebuffer_16bit_Diff";
  return true;
}
void animDraw(int f, bool full) { drawScene(*fb, f, full); }
void animPush() { fb->display(); }
void benchExtra() {
  // Zusätzlich: dieselben Animationen tearing-frei mit VSync
  if (fb && fb->setVSync(true)) {
    runAnim("Vollbild_VSync", true);
    runAnim("Ball_VSync", false);
    fb->setVSync(false);
  }
}

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
