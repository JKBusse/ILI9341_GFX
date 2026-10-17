// ILI9341_GFX: flicker-free animation with the framebuffer.
//
// Everything is drawn into RAM (150 KB at 16 bit); display() sends only the
// changed regions via DMA. With VSync the transfer follows the panel's
// refresh, so there is no tearing.
//
// Wiring (ESP32, VSPI): SCK 18, MOSI 23, MISO 19, CS 15, DC 2, RST 4, LED 13

#include <ILI9341_GFX.h>
#include <ILI9341_Framebuffer.h>

#define TFT_CS  15
#define TFT_DC  2
#define TFT_RST 4
#define TFT_BL  13

ILI9341_GFX tft(TFT_CS, TFT_DC, TFT_RST);
ILI9341_Framebuffer fb(tft); // ILI9341_Framebuffer8 needs half the RAM (RGB332/palette)

struct Ball {
  float x, y, vx, vy;
  int16_t r;
  uint16_t color;
};
Ball balls[12];

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.begin();
  if (!fb.begin()) {
    Serial.println("Not enough RAM for the framebuffer");
    while (true) delay(1000);
  }
  fb.setRotation(1);
  fb.setVSync(true); // needs MISO (reads the panel's scanline)

  for (Ball &b : balls) {
    b.r = random(6, 20);
    b.x = random(b.r, 320 - b.r);
    b.y = random(b.r, 240 - b.r);
    b.vx = random(-300, 300) / 100.0f;
    b.vy = random(-300, 300) / 100.0f;
    b.color = ILI9341_GFX::color565(random(80, 256), random(80, 256), random(80, 256));
  }
}

void loop() {
  static uint32_t frames = 0, t0 = millis(), fps = 0;

  fb.fillScreen(ILI9341_BLACK);
  for (Ball &b : balls) {
    b.x += b.vx;
    b.y += b.vy;
    if (b.x < b.r || b.x > 319 - b.r) b.vx = -b.vx;
    if (b.y < b.r || b.y > 239 - b.r) b.vy = -b.vy;
    fb.fillCircle(b.x, b.y, b.r, b.color);
  }
  fb.setCursor(4, 4);
  fb.setTextColor(ILI9341_WHITE);
  fb.printf("%lu fps", (unsigned long)fps);
  fb.display(); // send only what changed

  frames++;
  if (millis() - t0 >= 1000) {
    fps = frames;
    frames = 0;
    t0 = millis();
  }
}
