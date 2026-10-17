/*
 * ILI9341_Framebuffer - flackerfreies Zeichnen über einen Puffer im RAM.
 *
 * Man zeichnet den kompletten Frame mit allen Adafruit_GFX-Funktionen in den
 * Puffer (auch fillScreen() + alles neu) und ruft dann display() auf. Es
 * werden nur die Bildbereiche übertragen, die sich seit dem letzten Frame
 * tatsächlich geändert haben (Vergleich per Hash je 32-Pixel-Zeilensegment).
 * Jeder Pixel wird also genau einmal mit seinem finalen Wert geschrieben -
 * kein Flackern, und wenig Änderung = hohe Framerate.
 *
 * Zwei Varianten:
 *   ILI9341_Framebuffer   16 Bit/Pixel (RGB565), 150 KB
 *   ILI9341_Framebuffer8   8 Bit/Pixel, 75 KB. Farben werden wie gewohnt als
 *                          RGB565 übergeben und als RGB332 gespeichert - oder
 *                          mit setPalette() als Index in eine eigene Palette
 *                          aus 256 RGB565-Farben (z.B. für Palette-Cycling).
 *                          Die Umrechnung passiert beim Senden per Tabelle.
 *
 * Dazu kommen 10 KB Hashes. Der Puffer wird in Blöcke aufgeteilt, damit er
 * auch ohne PSRAM in den fragmentierten ESP32-Heap passt.
 */
#pragma once

#include "ILI9341_GFX.h"

template <typename T> class ILI9341_FramebufferT : public Adafruit_GFX {
public:
  explicit ILI9341_FramebufferT(ILI9341_GFX &tft);
  ~ILI9341_FramebufferT();

  /** Puffer anlegen. Nach tft.begin() aufrufen. false = nicht genug RAM. */
  bool begin();

  /** Überträgt alle geänderten Bereiche zum Display. */
  void display();
  /** Nächstes display() überträgt alles (z.B. nachdem direkt auf tft gezeichnet wurde). */
  void invalidate() { _forceAll = true; }
  /**
   * Kosten eines neuen Adressfensters in Pixel-Äquivalenten. display() fasst
   * Zeilen zu größeren Rechtecken zusammen, solange das billiger ist.
   */
  void setWindowCost(int32_t px) { _windowCost = px; }
#if defined(ESP32)
  /**
   * Tearing-freie Updates (braucht MISO): display() synchronisiert sich über
   * die Scanline des Panels mit dem Bildaufbau und schreibt immer direkt
   * hinter ihm her, in seiner Laufrichtung. Jeder Frame erscheint dadurch
   * vollständig in einem einzigen Refresh. Die Bildwiederholrate wird je
   * Frame gewählt (~92, ~55 oder ~48 Hz): die schnellste, bei der der Frame
   * laut Simulation sicher hinter dem Bildaufbau her übertragen werden kann.
   * Die Framerate ist auf die jeweilige Rate begrenzt.
   */
  bool setVSync(bool enable);
  bool vsync() const { return _vsync; }
  /** Streifen, die im VSync-Modus zu spät gesendet wurden (dort kann Tearing auftreten). */
  uint32_t vsyncLate() const { return _late; }
  float refreshHz() const { return _lineUs > 0 ? 1e6f / (_lineUs * _lines) : 0; }
#endif
  // ---- Hardware-Scrolling ----
  // Scrollachse: y im Hochformat, x im Querformat (die Richtung, in der das
  // Panel seine 320 Zeilen aufbaut). Verschoben wird im Display selbst - es
  // muss nur der frei werdende Streifen neu gezeichnet werden.

  /**
   * Bereiche am Anfang/Ende der Scrollachse (in Pixeln), die nicht mitscrollen,
   * z.B. eine Kopfzeile oder eine Achsenbeschriftung. Setzt den Scroll-Offset zurück.
   */
  void setScrollArea(int16_t fixedStart, int16_t fixedEnd);
  /**
   * Verschiebt den Inhalt des Scrollbereichs um delta Pixel zu kleineren
   * Koordinaten (nach oben bzw. links), zyklisch. Danach den frei gewordenen
   * Streifen am Ende neu zeichnen und display() aufrufen.
   */
  void scroll(int16_t delta);
#if defined(ESP32)
  /** Liest zurück, was auf dem Display in Zeile y zu sehen ist (für Tests, braucht MISO). */
  void readDisplayRow(int16_t y, uint16_t *out);
  /**
   * Screenshot (braucht MISO): schreibt "SHOT <w> <h>\n", dann w*h Pixel
   * RGB565 (MSB zuerst) und ein abschließendes "\n" nach out, z.B. Serial.
   * tools/tftctl.py --listen speichert das als PNG.
   */
  void screenshot(Print &out);
#endif

  /** Anzahl Pixel, die beim letzten display() übertragen wurden. */
  uint32_t lastPixelsSent() const { return _lastSent; }

  // ---- Adafruit_GFX-Overrides (alle schreiben nur in den RAM) ----
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void writePixel(int16_t x, int16_t y, uint16_t color) override;
  void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void fillScreen(uint16_t color) override;
  void writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override;
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override;
  // Schnelle Varianten (pixelgleich mit Adafruit_GFX; dort nicht virtuell,
  // daher gelten sie bei Aufrufen über den Framebuffer-Typ)
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  /** Textausgabe: identisch zu Adafruit_GFX, aber zeichnet ganze Läufe statt Einzelpixel. */
  size_t write(uint8_t c) override;
  using Print::write;
  void setRotation(uint8_t r) override;
  void invertDisplay(bool invert) override;

  /** RGB565-Bild in den Puffer kopieren (mit Clipping). */
  void pushImage(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *data);
  /** Farbe eines Pixels als RGB565 (bei 8 Bit: nach Palette bzw. RGB332). */
  uint16_t getPixel(int16_t x, int16_t y) const;
  /**
   * Direkter Zugriff auf eine Pufferzeile (Breite = width(), Rohformat T).
   * Markiert die ganze Zeile als geändert - den Zeiger daher nicht über ein
   * display() hinweg aufheben (sonst markDirty() verwenden).
   */
  T *getRow(int16_t y) {
    if (y < 0 || y >= _height) return nullptr;
    _touched[y] = ~0u;
    return _rows[y];
  }
  /** Bereich als geändert markieren (nach direktem Schreiben über einen alten getRow()-Zeiger). */
  void markDirty(int16_t x, int16_t y, int16_t w, int16_t h);

  /**
   * Nur 8 Bit: eigene Palette mit n RGB565-Farben setzen. Danach sind alle
   * Farbwerte Palettenindizes (0..255). Eine geänderte Palette wirkt beim
   * nächsten display() auf das ganze Bild (Palette-Cycling ohne Neuzeichnen).
   */
  void setPalette(const uint16_t *colors, uint16_t n = 256);
  /** Nur 8 Bit: zurück zu RGB565-Farben, gespeichert als RGB332 (Standard). */
  void useRGB332();
  /** RGB565 -> RGB332 (so speichert der 8-Bit-Puffer ohne Palette). */
  static constexpr uint8_t color332(uint16_t c565) {
    return ((c565 >> 13) << 5) | (((c565 >> 8) & 7) << 2) | ((c565 >> 3) & 3);
  }

private:
  static constexpr uint32_t TOTAL_PX = (uint32_t)ILI9341_TFTWIDTH * ILI9341_TFTHEIGHT;
  static constexpr uint8_t MAX_CHUNKS = 40;
  static constexpr uint8_t SEG_PX = 32; // Breite eines Hash-Segments (max. 32 Segmente je Zeile)
  static constexpr uint16_t MAX_SEGS = ILI9341_TFTHEIGHT * ((ILI9341_TFTWIDTH + SEG_PX - 1) / SEG_PX);

  void mapRows();
#if defined(ESP32)
  void displayVSync();
  struct RunU {
    int16_t u0, u1;
  };
  template <typename F> void forEachBand(F fn);
  bool _vsync = false;
  static constexpr uint8_t VSYNC_RATES = 3;
  float _lineUs = 0;                  // Zeilenzeit der aktuellen Rate
  float _rateLineUs[VSYNC_RATES] = {};
  uint8_t _rate = 0;                  // Index der aktuellen Rate (0 = schnellste)
  uint16_t _lines = 324;
  float calibrateLineUs();
  uint32_t _late = 0;
#endif
  void updateRow(int16_t y);
  void sendRect(int16_t x0, int16_t y0, int16_t w, int16_t h);
  void rebaseline();
  // Scrollbereich in Gate-Leitungen (Panel-Zeilen 0..319). _vsp ist der Offset,
  // zu dem der Puffer passt; _vspHw der, den das Display gerade anzeigt (im
  // VSync-Modus wird der neue Offset erst im nächsten display() umgeschaltet).
  uint16_t _tfa = 0, _vsa = ILI9341_TFTHEIGHT, _bfa = 0, _vsp = 0, _vspHw = 0;
  bool scrolled() const { return _vsp != _tfa; }
  /** Gate-Leitung -> Speicherzeile, die dort gerade angezeigt wird. */
  uint16_t memLine(uint16_t g) const {
    if (g < _tfa || g >= _tfa + _vsa) return g;
    return _tfa + (g - _tfa + (_vsp - _tfa)) % _vsa;
  }
  /** Logische Koordinate entlang der Scrollachse -> Adresse im Display-RAM. */
  int16_t memLogical(int16_t c) const {
    const int16_t N = ILI9341_TFTHEIGHT - 1;
    return rotation >= 2 ? N - memLine(N - c) : memLine(c);
  }
  void freeBuffers();

  ILI9341_GFX &_tft;
  T *_chunks[MAX_CHUNKS] = {};
  uint8_t _nChunks = 0;
  uint32_t _chunkPx = 0;
  T *_rows[ILI9341_TFTHEIGHT] = {};
  uint16_t *_lut = nullptr; // nur 8 Bit: Index -> RGB565, bereits byte-getauscht
  bool _indexed = false;    // nur 8 Bit: Farben sind Palettenindizes
  // Hash-Tabelle: bevorzugt in 5 DRAM-Blöcken (schnell), sonst ein Block im
  // 32-Bit-IRAM-Heap (gemessen ~20 % langsamer beim Hashen)
  static constexpr uint32_t HASH_CHUNK = 960; // Einträge, teilbar durch 15 und 20 Segmente
  static constexpr uint8_t HASH_CHUNKS = (MAX_SEGS + HASH_CHUNK - 1) / HASH_CHUNK;
  uint32_t *_hashChunks[HASH_CHUNKS] = {};
  bool _hashSingle = false;
  uint32_t *hashRow(int16_t y, int segs) {
    if (_hashSingle) return _hashChunks[0] + (uint32_t)y * segs;
    uint32_t rpc = HASH_CHUNK / segs;
    return _hashChunks[y / rpc] + (y % rpc) * segs;
  }
  uint32_t _rowMask[ILI9341_TFTHEIGHT]; // geänderte Segmente je Zeile (Bit n = Segment n)
  // Seit dem letzten display() beschriebene Segmente je Zeile: nur diese
  // müssen gehasht werden, der Rest ist garantiert unverändert
  uint32_t _touched[ILI9341_TFTHEIGHT] = {};
  static uint32_t segMask(int16_t x0, int16_t x1) { // x0..x1 inklusive, geclippt
    uint32_t a = x0 / SEG_PX, b = x1 / SEG_PX;
    uint32_t hi = b >= 31 ? 0xFFFFFFFFu : ((1u << (b + 1)) - 1);
    return hi & ~((1u << a) - 1);
  }
  inline void fillSpan(int16_t x, int16_t y, int16_t w, int16_t h, T c); // mit Clipping
  inline void hspan(int16_t x0, int16_t x1, int16_t y, T c);            // eine Zeile, x0..x1
  void fastChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg, uint8_t sx, uint8_t sy);

  struct Run {
    uint8_t s0, s1; // erstes und letztes Segment
  };
  uint8_t buildRuns(uint32_t mask, int32_t h, Run *runs) const;
  int32_t cost(uint32_t mask, int32_t h) const;
  bool _forceAll = true;
  int32_t _windowCost = 200; // gemessen: Fenster ~38 px, Optimum über alle Szenen bei 200
  uint32_t _lastSent = 0;
};

typedef ILI9341_FramebufferT<uint16_t> ILI9341_Framebuffer;
typedef ILI9341_FramebufferT<uint8_t> ILI9341_Framebuffer8;
