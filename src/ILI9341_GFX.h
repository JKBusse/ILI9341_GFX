/*
 * ILI9341_GFX - schneller ILI9341 SPI-Treiber, kompatibel zu Adafruit_GFX.
 *
 * Alle Zeichenfunktionen von Adafruit_GFX (Text, Fonts, Linien, Kreise,
 * Bitmaps, ...) funktionieren direkt. Die API orientiert sich an
 * Adafruit_ILI9341, damit bestehender Code weitgehend unverändert läuft.
 */
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>

// Schneller Pfad (Register, DMA, gepackter Modus) nur auf dem klassischen
// ESP32: er nutzt SPI3/VSPI und dessen GPIO-Matrix-Signale direkt. ESP32-S2/
// S3/C3/... und andere Plattformen laufen über die Arduino-SPI-Klasse.
#if defined(ESP32) && (!defined(CONFIG_IDF_TARGET) || defined(CONFIG_IDF_TARGET_ESP32))
#define ILI9341_GFX_ESP32 1
#else
#define ILI9341_GFX_ESP32 0
#endif

#if ILI9341_GFX_ESP32
#include <esp_intr_alloc.h>
struct lldesc_s;
#endif

#define ILI9341_TFTWIDTH  240
#define ILI9341_TFTHEIGHT 320

// Befehle
#define ILI9341_NOP        0x00
#define ILI9341_SWRESET    0x01
#define ILI9341_SLPIN      0x10
#define ILI9341_SLPOUT     0x11
#define ILI9341_PTLON      0x12
#define ILI9341_NORON      0x13
#define ILI9341_INVOFF     0x20
#define ILI9341_INVON      0x21
#define ILI9341_GAMMASET   0x26
#define ILI9341_DISPOFF    0x28
#define ILI9341_DISPON     0x29
#define ILI9341_CASET      0x2A
#define ILI9341_PASET      0x2B
#define ILI9341_RAMWR      0x2C
#define ILI9341_RAMRD      0x2E
#define ILI9341_PTLAR      0x30
#define ILI9341_IDMOFF     0x38
#define ILI9341_IDMON      0x39
#define ILI9341_VSCRDEF    0x33
#define ILI9341_MADCTL     0x36
#define ILI9341_VSCRSADD   0x37
#define ILI9341_PIXFMT     0x3A
#define ILI9341_FRMCTR1    0xB1
#define ILI9341_DFUNCTR    0xB6
#define ILI9341_PWCTR1     0xC0
#define ILI9341_PWCTR2     0xC1
#define ILI9341_VMCTR1     0xC5
#define ILI9341_VMCTR2     0xC7
#define ILI9341_RDMODE     0x0A
#define ILI9341_RDMADCTL   0x0B
#define ILI9341_RDPIXFMT   0x0C
#define ILI9341_RDIMGFMT   0x0D
#define ILI9341_RDSELFDIAG 0x0F
#define ILI9341_GMCTRP1    0xE0
#define ILI9341_GMCTRN1    0xE1

// MADCTL-Bits
#define ILI9341_MADCTL_MY  0x80
#define ILI9341_MADCTL_MX  0x40
#define ILI9341_MADCTL_MV  0x20
#define ILI9341_MADCTL_ML  0x10
#define ILI9341_MADCTL_BGR 0x08
#define ILI9341_MADCTL_MH  0x04

// RGB565-Farben (gleiche Namen wie Adafruit_ILI9341)
#ifndef ILI9341_BLACK
#define ILI9341_BLACK       0x0000
#define ILI9341_NAVY        0x000F
#define ILI9341_DARKGREEN   0x03E0
#define ILI9341_DARKCYAN    0x03EF
#define ILI9341_MAROON      0x7800
#define ILI9341_PURPLE      0x780F
#define ILI9341_OLIVE       0x7BE0
#define ILI9341_LIGHTGREY   0xC618
#define ILI9341_DARKGREY    0x7BEF
#define ILI9341_BLUE        0x001F
#define ILI9341_GREEN       0x07E0
#define ILI9341_CYAN        0x07FF
#define ILI9341_RED         0xF800
#define ILI9341_MAGENTA     0xF81F
#define ILI9341_YELLOW      0xFFE0
#define ILI9341_WHITE       0xFFFF
#define ILI9341_ORANGE      0xFD20
#define ILI9341_GREENYELLOW 0xAFE5
#define ILI9341_PINK        0xFC18
#endif

#if ILI9341_GFX_ESP32
#define ILI9341_GFX_DEFAULT_FREQ 40000000UL
#else
#define ILI9341_GFX_DEFAULT_FREQ 16000000UL
#endif

class ILI9341_GFX : public Adafruit_GFX {
public:
#if ILI9341_GFX_ESP32
  /**
   * ESP32: eigener Treiber direkt auf den VSPI-Registern (SPI3) mit DMA und
   * interruptgesteuerter Befehlsliste. Der Bus wird exklusiv belegt, die
   * Arduino-SPI-Klasse darf auf diesen Pins nicht genutzt werden.
   * Mit den Standardpins (SCK 18, MOSI 23, MISO 19) laufen die Signale über
   * IOMUX, damit sind bis zu 80 MHz möglich.
   *
   * @param cs   Chip-Select-Pin (-1, wenn CS fest auf GND liegt)
   * @param dc   Data/Command-Pin
   * @param rst  Reset-Pin (-1 = Software-Reset)
   */
  ILI9341_GFX(int8_t cs, int8_t dc, int8_t rst = -1, int8_t sck = 18, int8_t mosi = 23,
              int8_t miso = 19);
#else
  /**
   * @param cs   Chip-Select-Pin (-1, wenn CS fest auf GND liegt)
   * @param dc   Data/Command-Pin
   * @param rst  Reset-Pin (-1 = Software-Reset)
   * @param spi  SPI-Bus. Für eigene Pins vor begin() z.B.
   *             SPI.begin(sck, miso, mosi) aufrufen.
   */
  ILI9341_GFX(int8_t cs, int8_t dc, int8_t rst = -1, SPIClass *spi = &SPI);
#endif

#if ILI9341_GFX_ESP32
  /**
   * @param dmaBuffers Anzahl DMA-Puffer à 6,4 KB (2..16). Mehr Puffer = mehr
   *        Überlappung von Zeichnen und Übertragen, aber weniger freier Heap.
   *        Faustregel: Puffer * 6,4 KB sollten so viele Bytes fassen, wie in
   *        der Renderzeit eines Frames übertragen werden (40 MHz = 5 KB/ms).
   *        Mit WLAN eher 4 nehmen.
   */
  void begin(uint32_t freq = ILI9341_GFX_DEFAULT_FREQ, uint8_t dmaBuffers = 8);
  /** Wartet, bis alle DMA-Übertragungen fertig sind, und gibt den Bus frei. */
  void finishWrites();
  /** Tatsächlich angelegte DMA-Puffer (weniger als angefordert, wenn RAM knapp war). */
  uint8_t dmaBufferCount() const { return _numBufs; }
  /** Gibt DMA-Puffer frei, bis nur noch keep (mind. 2) übrig sind; wartet vorher alle Übertragungen ab. */
  void releaseDmaBuffers(uint8_t keep);
#else
  void begin(uint32_t freq = ILI9341_GFX_DEFAULT_FREQ);
  void finishWrites() {}
#endif
  void setSPISpeed(uint32_t freq);

  // ---- Adafruit_GFX-Overrides ----
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void startWrite() override;
  void endWrite() override;
  void writePixel(int16_t x, int16_t y, uint16_t color) override;
  void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void fillScreen(uint16_t color) override;
  void setRotation(uint8_t r) override;
  void invertDisplay(bool invert) override;
  // Schnelle Varianten, pixelgleich mit Adafruit_GFX: Linien und Kreise als
  // Strecken statt Einzelpixel, Text mit einem Fenster je Zeichen
  void writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override;
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void drawCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corners, uint16_t color);
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  size_t write(uint8_t c) override;
  using Print::write;

  // ---- Low-Level (innerhalb startWrite()/endWrite() verwenden) ----
  // Auf dem ESP32 laufen alle Schreibzugriffe asynchron per DMA. Die Daten
  // werden vorher in interne Puffer kopiert, der Aufrufer darf seinen Speicher
  // also sofort weiterverwenden. endWrite() wartet NICHT auf das Ende der
  // Übertragung (dafür finishWrites()); CS bleibt bis dahin aktiv.

  /** Setzt das Schreibfenster und startet RAMWR. Keine Clipping-Prüfung. */
  void setAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  /** Schreibt len Pixel. bigEndian=true, wenn die Daten bereits MSB-first vorliegen. */
  void writePixels(const uint16_t *colors, uint32_t len, bool bigEndian = false);
  /** Schreibt len-mal dieselbe Farbe. */
  void writeColor(uint16_t color, uint32_t len);
  void writeCommand(uint8_t cmd);
  void spiWrite(uint8_t b);
  void spiWrite16(uint16_t w);

  // ---- Höhere Funktionen ----
  /** Schnelles Blitten eines RGB565-Puffers (mit Clipping). */
  void pushImage(int16_t x, int16_t y, int16_t w, int16_t h,
                 const uint16_t *data, bool bigEndian = false);
  /** Zeichnet einen GFXcanvas16 als Ganzes (für flackerfreies Double-Buffering). */
  void drawCanvas(int16_t x, int16_t y, GFXcanvas16 &canvas);

  void sendCommand(uint8_t cmd, const uint8_t *data = nullptr, uint8_t len = 0);
  uint8_t readcommand8(uint8_t cmd, uint8_t index = 0);
#if ILI9341_GFX_ESP32
  /** Liest einen Bereich aus dem Display-RAM (braucht MISO). */
  void readRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *out);
  /** Sendet cmd und liest danach len Bytes roh zurück (inkl. evtl. Dummy-Byte). */
  void readRegister(uint8_t cmd, uint8_t *buf, uint8_t len);
  /** Aktuelle Scanline des Panels (Get_Scanline, 45h), 0 = Beginn VSYNC. */
  uint16_t getScanline();

  // ---- Für eigene Pixel-Generatoren (z.B. VSync-Übertragung) ----
  /**
   * Liefert Platz für bis zu maxPx Pixel im aktuellen DMA-Puffer; n = tatsächlich
   * verfügbare Anzahl. Die Pixel müssen bereits MSB-first (byte-getauscht)
   * geschrieben werden. Danach commitPixels(n) aufrufen.
   */
  uint16_t *pixelSpace(uint32_t maxPx, uint32_t &n);
  void commitPixels(uint32_t n);
  /** Bytes, die noch in der Befehlsliste auf Übertragung warten. */
  uint32_t pendingBytes() const { return _pendingBytes; }
  /** Bildwiederholrate (FRMCTR1, B1h): Teiler DIVA 0..3, Takte pro Zeile RTNA 16..31. */
  void setFrameRate(uint8_t diva, uint8_t rtna);
  /**
   * Gepackter Modus (Standard: an): kleine Fenster samt Pixeln (CASET, PASET,
   * RAMWR, Farbe) gehen als EINE SPI-Übertragung raus. Die SPI-Einheit läuft
   * dafür im Dual-Modus: die zweite Datenleitung wird über die GPIO-Matrix
   * auf den DC-Pin gelegt und liefert zu jedem Bit den D/CX-Pegel. Spart bei
   * Linien, Kreisen und Text bis zu 5 von 6 Übertragungen je Strecke.
   */
  void setPacked(bool on);
  bool packed() const { return _packed; }
  /** Prüft den gepackten Modus per Readback (braucht MISO und 2 KB Heap); Rückgabe: falsche Pixel, UINT32_MAX = kein RAM. */
  uint32_t verifyPacked(uint32_t seed = 1);
  /** Panel-Reset und Init-Sequenz erneut (Inhalt geht verloren). */
  void reinitPanel();
#endif
  /** MADCTL direkt setzen (z.B. native Adressierung); invalidiert den Adress-Cache. */
  void setMadctl(uint8_t value);
  /** MADCTL-Wert der aktuellen Rotation. */
  uint8_t rotationMadctl() const;
  uint32_t spiSpeed() const { return _freq; }
#if ILI9341_GFX_ESP32
#endif

  void setScrollMargins(uint16_t top, uint16_t bottom);
  void scrollTo(uint16_t y);
  void enableDisplay(bool enable);
  /** Schlafmodus (~ein paar µA im Panel). Inhalt bleibt erhalten; wartet die nötigen 120 ms. */
  void enableSleep(bool enable);

  // ---- Stromsparen ----
  /** Idle-Modus: nur noch 8 Farben (oberstes Bit je Kanal), spart Strom im Panel. */
  void setIdleMode(bool enable);
  /**
   * Teilanzeige: nur die Panel-Zeilen first..last (0..319, native Zeilen =
   * Hochformat-y) werden angezeigt, der Rest wird schwarz bzw. weiß (je nach
   * Panel) und nicht mehr aufgefrischt. enable=false: wieder ganzes Bild.
   * Hinweis: Teilanzeige und Hardware-Scrolling schließen sich laut Datenblatt
   * aus; beim Verlassen werden die Scroll-Einstellungen wiederhergestellt.
   */
  void setPartialMode(bool enable, uint16_t first = 0, uint16_t last = ILI9341_TFTHEIGHT - 1);
#if ILI9341_GFX_ESP32
  /**
   * Ermittelt den höchsten fehlerfreien SPI-Takt (braucht MISO): schreibt für
   * jeden Kandidaten Zufallsdaten und viele kleine Fenster und prüft sie per
   * Readback. Initialisiert das Panel danach neu (Bildinhalt geht verloren)
   * und stellt den gefundenen Takt ein. Rückgabe: Takt in Hz, 0 = nichts lief.
   */
  uint32_t autoSpeed(uint32_t maxFreq = 80000000);
#endif

  static constexpr uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

private:
  inline void csLow();
  inline void csHigh();
  void hwReset();
  void invalidateAddrCache();
  void writeAddrCmd(uint8_t cmd, uint16_t a, uint16_t b);
  void fillRectFast(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color); // geclippt, w,h > 0
  void hline(int16_t x, int16_t y, int16_t w, uint16_t color);
  void vline(int16_t x, int16_t y, int16_t h, uint16_t color);
  void circleRuns(int16_t x0, int16_t y0, int16_t r, uint8_t corners, uint16_t color);
  void fastChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg, uint8_t sx, uint8_t sy);

  int8_t _cs, _dc, _rst;
  uint8_t _txDepth = 0;
  uint16_t _scrTop = 0, _scrBottom = 0, _scrVsp = 0; // zuletzt gesetzte Scroll-Register
  uint32_t _freq = ILI9341_GFX_DEFAULT_FREQ;

  // Zuletzt gesetzte Spalten/Zeilen, um redundante CASET/PASET zu sparen
  uint16_t _cx0, _cx1, _cy0, _cy1;

#if ILI9341_GFX_ESP32
public:
  void isrHandler(); // nur für die Interrupt-Routine

private:
  static constexpr uint8_t NUM_OPS = 64;      // Länge der Befehlsliste
  static constexpr uint8_t MAX_BUFS = 16;
  static constexpr uint32_t BUF_BYTES = 6400; // 10 Zeilen à 320 Pixel
  static constexpr uint8_t INLINE_BYTES = 16; // kleine Transfers über die SPI-Register

  // Ein Eintrag der Befehlsliste: entweder bis zu 16 Byte direkt, oder ein DMA-Puffer
  struct Op {
    uint16_t len;
    uint8_t dc;
    uint8_t buf; // 0xFF = Daten in data[], PACKED_OP = gepackte Strecke in data[]
    uint32_t data[INLINE_BYTES / 4];
  };

  void configureHw();
  void startOp(const Op &op);
  uint32_t submit(const Op &op);
  uint32_t submitSmall(const uint8_t *data, uint32_t len, bool dc);
  void pollWrite(const uint8_t *data, uint32_t len, bool dc);
  void pollRead(uint8_t *buf, uint32_t len);
  void appendBytes(const uint8_t *data, uint32_t len);
  void flushBuffer();
  void waitFor(uint32_t seq);
  void waitIdle() { waitFor(_submitted); }
  uint8_t *acquireBuffer();

  int8_t _sck, _mosi, _miso;
  volatile uint32_t *_csSet = nullptr, *_csClr = nullptr;
  volatile uint32_t *_dcSet = nullptr, *_dcClr = nullptr;
  uint32_t _csMask = 0, _dcMask = 0;
  uint32_t _clkWrite = 0, _clkRead = 0;
  intr_handle_t _intr = nullptr;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

  Op _ops[NUM_OPS];
  volatile uint32_t _opHead = 0, _opTail = 0;       // Ringpuffer der Befehlsliste
  volatile uint32_t _submitted = 0, _doneSeq = 0;   // fortlaufende Nummern
  volatile bool _busy = false;
  volatile uint32_t _pendingBytes = 0;

  // Schneller Registerweg (wie LovyanGFX): bei leerer Befehlsliste gehen
  // Kommandos und kleine Daten direkt über W0..W15 raus, ohne Interrupt und
  // ohne auf das Ende zu warten
  volatile bool _dmaDirty = true; // DMA lief seit dem letzten Register-Transfer
  bool _intOn = true;             // SPI-Interrupt (für die Befehlsliste) aktiv
  inline bool directOk() const { return !_busy && !_bufFill; }
  void enterDirect();
  void leaveDirect();
  void directBegin(bool dc);
  void directWrite(bool dc, uint32_t w0, uint32_t bits);
  void directFill(uint16_t swapped, uint32_t len);
  void directWindow(uint16_t x, uint16_t y, uint16_t x1, uint16_t y1);
  void packedFill(uint16_t x, uint16_t y, uint16_t x1, uint16_t y1, uint16_t color, uint32_t n);
  static constexpr uint8_t PACKED_OP = 0xFE;
  static constexpr uint32_t WAIT_BYTES = 8192;        // kürzerer Listenrest: abwarten, dann direkt
  static constexpr uint32_t QUEUE_PACK_BYTES = 4096;  // ab diesem Rückstau auch lange Strecken gepackt einreihen
  uint32_t queuePacked(uint16_t x, uint16_t y, uint16_t x1, uint16_t y1, uint16_t color, uint32_t n);
  void startPacked(const Op &op);
  void dualOn();
  void dualOff(uint32_t user);
  void leaveDual();
  bool _packed = false;
  volatile bool _dual = false; // SPI im Dual-Modus, DC hängt an der Q-Leitung

  struct lldesc_s *_desc = nullptr; // 2 DMA-Deskriptoren je Puffer
  uint8_t *_buf[MAX_BUFS] = {};
  uint32_t _bufSeq[MAX_BUFS] = {};  // letzte Übertragung, die den Puffer liest
  uint8_t _numBufs = 0;
  uint8_t _bufIdx = 0;
  bool _busHeld = false;
  uint32_t _bufFill = 0;
#else
  inline void dcLow();
  inline void dcHigh();

  SPIClass *_spi;
  SPISettings _settings;
#endif
};
