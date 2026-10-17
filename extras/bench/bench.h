// Gemeinsamer Benchmark für ILI9341-Bibliotheken (angelehnt an Adafruit graphicstest).
//
// Die einbindende Datei definiert vorher:
//   DISP                          das Display-Objekt (Adafruit-ähnliche API)
//   const char *LIB_NAME
//   void benchInit()              Display starten, Rotation 1, 40 MHz
//   void benchSync()              warten, bis alles wirklich gesendet ist (DMA)
//   void benchPushStrip(int16_t y, uint16_t *buf)   320x40-Pixel-Streifen senden
//   bool animBegin(String &info)  Off-Screen-Puffer 320x240 anlegen (false = geht nicht)
//   void animDraw(int frame, bool full)  Szene in den Puffer zeichnen
//   void animPush()               Puffer anzeigen
#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>

void benchExtra(); // zusätzliche Tests einer Bibliothek (optional leer)

#define C_BLACK 0x0000
#define C_BLUE 0x001F
#define C_RED 0xF800
#define C_GREEN 0x07E0
#define C_CYAN 0x07FF
#define C_MAGENTA 0xF81F
#define C_YELLOW 0xFFE0
#define C_WHITE 0xFFFF

static void report(const char *test, uint32_t us) {
  Serial.printf("RES %s %s %lu\n", LIB_NAME, test, us);
}

template <class D> uint32_t testFillScreen(D &d) {
  uint32_t s = micros();
  d.fillScreen(C_BLACK);
  d.fillScreen(C_RED);
  d.fillScreen(C_GREEN);
  d.fillScreen(C_BLUE);
  d.fillScreen(C_BLACK);
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testText(D &d) {
  d.fillScreen(C_BLACK);
  benchSync();
  uint32_t s = micros();
  d.setCursor(0, 0);
  d.setTextColor(C_WHITE);
  d.setTextSize(1);
  d.println("Hello World!");
  d.setTextColor(C_YELLOW);
  d.setTextSize(2);
  d.println(1234.56);
  d.setTextColor(C_RED);
  d.setTextSize(3);
  d.println(0xDEADBEEF, HEX);
  d.println();
  d.setTextColor(C_GREEN);
  d.setTextSize(5);
  d.println("Groop");
  d.setTextSize(2);
  d.println("I implore thee,");
  d.setTextSize(1);
  d.println("my foonting turlingdromes.");
  d.println("And hooptiously drangle me");
  d.println("with crinkly bindlewurdles,");
  d.println("Or I will rend thee");
  d.println("in the gobberwarts");
  d.println("with my blurglecruncheon,");
  d.println("see if I don't!");
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testLines(D &d, uint16_t color) {
  int x1, y1, x2, y2, w = d.width(), h = d.height();
  uint32_t t = 0, s;
  d.fillScreen(C_BLACK);
  benchSync();
  x1 = y1 = 0;
  y2 = h - 1;
  s = micros();
  for (x2 = 0; x2 < w; x2 += 6) d.drawLine(x1, y1, x2, y2, color);
  x2 = w - 1;
  for (y2 = 0; y2 < h; y2 += 6) d.drawLine(x1, y1, x2, y2, color);
  benchSync();
  t += micros() - s;
  d.fillScreen(C_BLACK);
  benchSync();
  x1 = w - 1;
  y1 = 0;
  y2 = h - 1;
  s = micros();
  for (x2 = 0; x2 < w; x2 += 6) d.drawLine(x1, y1, x2, y2, color);
  x2 = 0;
  for (y2 = 0; y2 < h; y2 += 6) d.drawLine(x1, y1, x2, y2, color);
  benchSync();
  t += micros() - s;
  d.fillScreen(C_BLACK);
  benchSync();
  x1 = 0;
  y1 = h - 1;
  y2 = 0;
  s = micros();
  for (x2 = 0; x2 < w; x2 += 6) d.drawLine(x1, y1, x2, y2, color);
  x2 = w - 1;
  for (y2 = 0; y2 < h; y2 += 6) d.drawLine(x1, y1, x2, y2, color);
  benchSync();
  t += micros() - s;
  d.fillScreen(C_BLACK);
  benchSync();
  x1 = w - 1;
  y1 = h - 1;
  y2 = 0;
  s = micros();
  for (x2 = 0; x2 < w; x2 += 6) d.drawLine(x1, y1, x2, y2, color);
  x2 = 0;
  for (y2 = 0; y2 < h; y2 += 6) d.drawLine(x1, y1, x2, y2, color);
  benchSync();
  return t + micros() - s;
}

template <class D> uint32_t testFastLines(D &d, uint16_t c1, uint16_t c2) {
  int w = d.width(), h = d.height();
  d.fillScreen(C_BLACK);
  benchSync();
  uint32_t s = micros();
  for (int y = 0; y < h; y += 5) d.drawFastHLine(0, y, w, c1);
  for (int x = 0; x < w; x += 5) d.drawFastVLine(x, 0, h, c2);
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testRects(D &d, uint16_t color) {
  int cx = d.width() / 2, cy = d.height() / 2, n = min(d.width(), d.height());
  d.fillScreen(C_BLACK);
  benchSync();
  uint32_t s = micros();
  for (int i = 2; i < n; i += 6) {
    int i2 = i / 2;
    d.drawRect(cx - i2, cy - i2, i, i, color);
  }
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testFilledRects(D &d, uint16_t c1, uint16_t c2) {
  int cx = d.width() / 2 - 1, cy = d.height() / 2 - 1, n = min(d.width(), d.height());
  uint32_t t = 0;
  d.fillScreen(C_BLACK);
  benchSync();
  for (int i = n - 1; i > 0; i -= 6) {
    int i2 = i / 2;
    uint32_t s = micros();
    d.fillRect(cx - i2, cy - i2, i, i, c1);
    benchSync();
    t += micros() - s;
    d.drawRect(cx - i2, cy - i2, i, i, c2); // nicht gezählt (wie graphicstest)
    benchSync();
  }
  return t;
}

template <class D> uint32_t testFilledCircles(D &d, uint8_t r, uint16_t color) {
  int w = d.width(), h = d.height(), r2 = r * 2;
  d.fillScreen(C_BLACK);
  benchSync();
  uint32_t s = micros();
  for (int x = r; x < w; x += r2)
    for (int y = r; y < h; y += r2) d.fillCircle(x, y, r, color);
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testCircles(D &d, uint8_t r, uint16_t color) {
  int w = d.width() + r, h = d.height() + r, r2 = r * 2;
  uint32_t s = micros();
  for (int x = 0; x < w; x += r2)
    for (int y = 0; y < h; y += r2) d.drawCircle(x, y, r, color);
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testTriangles(D &d) {
  int cx = d.width() / 2 - 1, cy = d.height() / 2 - 1, n = min(cx, cy);
  d.fillScreen(C_BLACK);
  benchSync();
  uint32_t s = micros();
  for (int i = 0; i < n; i += 5)
    d.drawTriangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, ((i * 7) & 0x1F) << 11 | ((i * 3) & 0x3F) << 5);
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testFilledTriangles(D &d) {
  int cx = d.width() / 2 - 1, cy = d.height() / 2 - 1;
  uint32_t t = 0;
  d.fillScreen(C_BLACK);
  benchSync();
  for (int i = min(cx, cy); i > 10; i -= 5) {
    uint32_t s = micros();
    d.fillTriangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, (i & 0x1F) | (((i * 2) & 0x3F) << 5));
    benchSync();
    t += micros() - s;
    d.drawTriangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, ((i * 5) & 0x1F) << 11);
    benchSync();
  }
  return t;
}

template <class D> uint32_t testRoundRects(D &d) {
  int cx = d.width() / 2 - 1, cy = d.height() / 2 - 1, w = min(d.width(), d.height());
  d.fillScreen(C_BLACK);
  benchSync();
  uint32_t s = micros();
  for (int i = 0; i < w; i += 6) {
    int i2 = i / 2;
    d.drawRoundRect(cx - i2, cy - i2, i, i, i / 8, (i & 0x1F) << 11);
  }
  benchSync();
  return micros() - s;
}

template <class D> uint32_t testFilledRoundRects(D &d) {
  int cx = d.width() / 2 - 1, cy = d.height() / 2 - 1;
  d.fillScreen(C_BLACK);
  benchSync();
  uint32_t s = micros();
  for (int i = min(d.width(), d.height()); i > 20; i -= 6) {
    int i2 = i / 2;
    d.fillRoundRect(cx - i2, cy - i2, i, i, i / 8, (i & 0x3F) << 5);
  }
  benchSync();
  return micros() - s;
}

// Vollbild aus dem RAM senden: 6 Streifen à 320x40
static uint32_t testPushImage() {
  static uint16_t *buf = nullptr;
  if (!buf) buf = (uint16_t *)malloc(320 * 40 * 2);
  if (!buf) return 0;
  for (int i = 0; i < 320 * 40; i++) buf[i] = i * 37;
  benchSync();
  const int N = 5;
  uint32_t s = micros();
  for (int n = 0; n < N; n++)
    for (int y = 0; y < 240; y += 40) benchPushStrip(y, buf);
  benchSync();
  return (micros() - s) / N;
}

// Szene für die Animationstests: full = Vollbild-Szene, sonst nur ein bewegter Ball
template <class S> void drawScene(S &s, int frame, bool full) {
  if (full) {
    s.fillScreen(0x1082);
    for (int i = 0; i < 20; i++) {
      int x = 160 + (int)(120 * sinf(frame * 0.05f + i)), y = 120 + (int)(90 * cosf(frame * 0.07f + i * 1.3f));
      s.fillCircle(x, y, 12, (uint16_t)(i * 3301));
    }
    s.setTextColor(C_WHITE);
    s.setTextSize(2);
    s.setCursor(8, 8);
    s.print("Frame ");
    s.print(frame);
  } else {
    // statischer Hintergrund, nur ein Ball bewegt sich
    static int lastX = -100;
    if (frame == 0) {
      s.fillScreen(0x1082);
      for (int y = 0; y < 240; y += 20) s.fillRect(0, y, 320, 10, 0x2104);
    } else {
      int ly = 120;
      s.fillRect(lastX - 13, ly - 13, 27, 27, 0x1082); // alten Ball löschen
      for (int y = (ly - 13) / 20 * 20; y < ly + 14; y += 20) s.fillRect(lastX - 13, y, 27, 10, 0x2104);
    }
    int x = 20 + (frame * 3) % 280;
    s.fillCircle(x, 120, 12, C_YELLOW);
    lastX = x;
  }
}

static void runAnim(const char *name, bool full) {
  String info;
  if (!animBegin(info)) {
    Serial.printf("ANIM %s %s 0 %s\n", LIB_NAME, name, info.c_str());
    return;
  }
  int frames = 0;
  uint32_t s = millis();
  while (millis() - s < 3000) {
    animDraw(frames, full);
    animPush();
    frames++;
  }
  benchSync();
  float fps = frames * 1000.0f / (millis() - s);
  Serial.printf("ANIM %s %s %.1f %s\n", LIB_NAME, name, fps, info.c_str());
}

template <class D> void runAll(D &d) {
  report("FillScreen", testFillScreen(d));
  report("Text", testText(d));
  report("Lines", testLines(d, C_CYAN));
  report("HV-Lines", testFastLines(d, C_RED, C_BLUE));
  report("Rects", testRects(d, C_GREEN));
  report("FilledRects", testFilledRects(d, C_YELLOW, C_MAGENTA));
  report("FilledCircles", testFilledCircles(d, 10, C_MAGENTA));
  report("Circles", testCircles(d, 10, C_WHITE));
  report("Triangles", testTriangles(d));
  report("FilledTriangles", testFilledTriangles(d));
  report("RoundRects", testRoundRects(d));
  report("FilledRoundRects", testFilledRoundRects(d));
  report("PushImage320x240", testPushImage());
  runAnim("Vollbild", true);
  runAnim("Ball", false);
  benchExtra();
  Serial.printf("HEAP %s %lu %u\n", LIB_NAME, ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  Serial.println("DONE");
}
