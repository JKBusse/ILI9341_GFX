// ILI9341_GFX – Grundlagen: direkt auf das Display zeichnen.
// Alle Funktionen von Adafruit_GFX stehen zur Verfügung.
//
// Verdrahtung (ESP32, VSPI): SCK 18, MOSI 23, MISO 19, CS 15, DC 2, RST 4, LED 13

#include <ILI9341_GFX.h>
#include <Fonts/FreeSansBold12pt7b.h>

#define TFT_CS  15
#define TFT_DC  2
#define TFT_RST 4
#define TFT_BL  13 // Hintergrundbeleuchtung (-1, wenn fest verdrahtet)

ILI9341_GFX tft(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }
  tft.begin(); // 40 MHz
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);

  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(10, 30);
  tft.print("ILI9341_GFX");
  tft.setFont();

  for (int i = 0; i < 16; i++) tft.drawLine(0, 60, i * 20, 239, tft.color565(i * 16, 255 - i * 16, 128));
  tft.fillRoundRect(200, 50, 110, 60, 10, ILI9341_NAVY);
  tft.drawRoundRect(200, 50, 110, 60, 10, ILI9341_CYAN);
  tft.fillCircle(255, 170, 40, ILI9341_ORANGE);
  tft.drawCircle(255, 170, 50, ILI9341_YELLOW);
}

void loop() {
  // Zähler mit Hintergrundfarbe: überschreibt die alte Zahl ohne Flackern
  static uint32_t n = 0;
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_GREEN, ILI9341_NAVY);
  tft.setCursor(215, 72);
  tft.printf("%6lu", (unsigned long)n++);
  delay(50);
}
