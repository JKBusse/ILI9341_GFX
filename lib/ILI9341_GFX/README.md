# ILI9341_GFX (library)

A fast ILI9341 SPI driver for the classic ESP32. It is compatible with Adafruit_GFX and comes with a framebuffer, tear-free VSync and hardware scrolling.

- `ILI9341_GFX.h`: the display driver, which draws directly on the display.
- `ILI9341_Framebuffer.h`: a framebuffer with dirty-region updates. `ILI9341_Framebuffer` is 16 bit, `ILI9341_Framebuffer8` stores RGB332 or palette indexes.
- `examples/Basic`: direct drawing.
- `examples/Framebuffer`: flicker-free animation with VSync.

Dependency: [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) ≥ 1.11.9.

The [project README](../../README.md) covers features, hardware requirements, the API, benchmarks and a comparison with other libraries.
