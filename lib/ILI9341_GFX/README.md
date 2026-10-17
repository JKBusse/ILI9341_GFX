# ILI9341_GFX (Bibliothek)

Schneller ILI9341-SPI-Treiber für den ESP32, kompatibel zu Adafruit_GFX, mit Framebuffer, VSync und Hardware-Scrolling.

- `ILI9341_GFX.h`: Treiber, zeichnet direkt aufs Display.
- `ILI9341_Framebuffer.h`: Framebuffer mit Dirty-Rect-Übertragung. `ILI9341_Framebuffer` arbeitet mit 16 Bit, `ILI9341_Framebuffer8` mit RGB332 oder Palette.
- `examples/Basic`: direktes Zeichnen.
- `examples/Framebuffer`: flackerfreie Animation mit VSync.

Abhängigkeit: [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) ≥ 1.11.9.

Dokumentation, Verdrahtung, API und Benchmarks stehen im [README des Projekts](../../README.md).
