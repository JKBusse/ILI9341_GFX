// ILI9341_GFX – flackerfreie Animation mit Framebuffer.
//
// Gezeichnet wird ins RAM (150 KB bei 16 Bit), display() überträgt nur die
// geänderten Bereiche per DMA. Mit VSync wird hinter dem Bildaufbau des
// Panels geschrieben: kein Tearing.
//
// Verdrahtung (ESP32, VSPI): SCK 18, MOSI 23, MISO 19, CS 15, DC 2, RST 4, LED 13

#include <ILI9341_GFX.h>
#include <ILI9341_Framebuffer.h>

#define TFT_CS  15
#define TFT_DC  2
#define TFT_RST 4
#define TFT_BL  13

ILI9341_GFX tft(TFT_CS, TFT_DC, TFT_RST);
ILI9341_Framebuffer fb(tft); // ILI9341_Framebuffer8 braucht nur die Hälfte (RGB332/Palette)

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
    Serial.println("Nicht genug RAM für den Framebuffer");
    while (true) delay(1000);
  }
  fb.setRotation(1);
  fb.setVSync(true); // braucht MISO (liest die Scanline des Panels)

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
  fb.display(); // nur Geändertes übertragen

  frames++;
  if (millis() - t0 >= 1000) {
    fps = frames;
    frames = 0;
    t0 = millis();
  }
}
