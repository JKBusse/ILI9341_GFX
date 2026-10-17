#include "ILI9341_Framebuffer.h"

#include <algorithm>

// Klassischer 5x7-Font von Adafruit_GFX (dort static, daher hier eingebunden)
#include "glcdfont.c"

#if defined(ESP32)
#include "esp_heap_caps.h"
#endif

// Blockgrößen sind Vielfache von 960 px (= kgV von 240 und 320), damit in
// jeder Rotation ganze Zeilen in einen Block passen.
static const uint32_t CHUNK_SIZES[] = {19200, 9600, 4800, 1920};

static inline void fillPx(uint16_t *p, uint16_t c, int32_t n) {
  if ((c >> 8) == (c & 0xFF)) {
    memset(p, c & 0xFF, n * 2);
    return;
  }
  if (((uintptr_t)p & 2) && n) {
    *p++ = c;
    n--;
  }
  uint32_t c2 = ((uint32_t)c << 16) | c;
  uint32_t *q = (uint32_t *)p;
  for (int32_t i = n >> 1; i; i--) *q++ = c2;
  if (n & 1) *(uint16_t *)q = c;
}
static inline void fillPx(uint8_t *p, uint8_t c, int32_t n) { memset(p, c, n); }

// Farbe (RGB565 bzw. Palettenindex) -> gespeicherter Pixel
static inline uint16_t encodePx(uint16_t c, bool, uint16_t *) { return c; }
static inline uint8_t encodePx(uint16_t c, bool indexed, uint8_t *) {
  return indexed ? (uint8_t)c : ILI9341_FramebufferT<uint8_t>::color332(c);
}

// Gespeicherter Pixel -> RGB565 byte-getauscht (so wie er übertragen wird)
static inline uint16_t swappedPx(uint16_t c, const uint16_t *) { return (c << 8) | (c >> 8); }
static inline uint16_t swappedPx(uint8_t c, const uint16_t *lut) { return lut[c]; }

// Pixel einer Zeile senden
static inline void emitPx(ILI9341_GFX &tft, const uint16_t *src, uint32_t n, const uint16_t *) {
  tft.writePixels(src, n);
}
static void emitPx(ILI9341_GFX &tft, const uint8_t *src, uint32_t n, const uint16_t *lut) {
#if defined(ESP32)
  // Direkt in den DMA-Puffer umrechnen
  while (n) {
    uint32_t cnt;
    uint16_t *dst = tft.pixelSpace(n, cnt);
    for (uint32_t j = 0; j < cnt; j++) dst[j] = lut[src[j]];
    tft.commitPixels(cnt);
    src += cnt;
    n -= cnt;
  }
#else
  uint16_t tmp[32];
  while (n) {
    uint32_t k = n > 32 ? 32 : n;
    for (uint32_t j = 0; j < k; j++) tmp[j] = lut[src[j]];
    tft.writePixels(tmp, k, true);
    src += k;
    n -= k;
  }
#endif
}

// FNV-1a über Bytes (nur für ein Restsegment bei ungewöhnlicher Breite)
static inline uint32_t hashBytes(const uint8_t *p, int n) {
  uint32_t h = 0x811C9DC5;
  while (n--) h = (h ^ *p++) * 0x01000193;
  return h;
}

template <typename T>
ILI9341_FramebufferT<T>::ILI9341_FramebufferT(ILI9341_GFX &tft)
    : Adafruit_GFX(ILI9341_TFTWIDTH, ILI9341_TFTHEIGHT), _tft(tft) {}

template <typename T> ILI9341_FramebufferT<T>::~ILI9341_FramebufferT() { freeBuffers(); }

template <typename T>
void ILI9341_FramebufferT<T>::freeBuffers() {
  for (uint8_t i = 0; i < _nChunks; i++) free(_chunks[i]);
  _nChunks = 0;
  for (uint8_t i = 0; i < HASH_CHUNKS; i++) {
    free(_hashChunks[i]);
    _hashChunks[i] = nullptr;
  }
  _hashSingle = false;
  free(_lut);
  _lut = nullptr;
}

template <typename T> void ILI9341_FramebufferT<T>::setPalette(const uint16_t *colors, uint16_t n) {
  if (sizeof(T) != 1 || !_lut) return;
  for (uint16_t i = 0; i < 256; i++) {
    uint16_t c = i < n ? colors[i] : 0;
    _lut[i] = (c << 8) | (c >> 8);
  }
  _indexed = true;
  _forceAll = true;
}

template <typename T> void ILI9341_FramebufferT<T>::useRGB332() {
  if (sizeof(T) != 1 || !_lut) return;
  for (uint16_t i = 0; i < 256; i++) {
    uint16_t r3 = i >> 5, g3 = (i >> 2) & 7, b2 = i & 3;
    uint16_t c = ((r3 << 2 | r3 >> 1) << 11) | ((g3 << 3 | g3) << 5) | (b2 << 3 | b2 << 1 | b2 >> 1);
    _lut[i] = (c << 8) | (c >> 8);
  }
  _indexed = false;
  _forceAll = true;
}

template <typename T>
bool ILI9341_FramebufferT<T>::begin() {
  if (_nChunks) return true;

#if defined(ESP32)
  if (psramFound()) {
    _chunks[0] = (T *)ps_malloc(TOTAL_PX * sizeof(T));
    if (_chunks[0]) {
      _nChunks = 1;
      _chunkPx = TOTAL_PX;
    }
  }
#endif

  for (uint32_t size : CHUNK_SIZES) {
    if (_nChunks) break;
    uint8_t n = TOTAL_PX / size, i = 0;
    for (; i < n; i++) {
      _chunks[i] = (T *)malloc(size * sizeof(T));
      if (!_chunks[i]) break;
    }
    if (i == n) {
      _nChunks = n;
      _chunkPx = size;
    } else {
      while (i) free(_chunks[--i]);
    }
  }

  if (!_nChunks) {
    freeBuffers();
    return false;
  }

  // Hash-Tabelle nach dem Bildpuffer anlegen (der braucht die großen Blöcke)
  uint8_t h = 0;
  for (; h < HASH_CHUNKS; h++) {
    _hashChunks[h] = (uint32_t *)malloc(HASH_CHUNK * sizeof(uint32_t));
    if (!_hashChunks[h]) break;
  }
  if (h < HASH_CHUNKS) {
    while (h) {
      free(_hashChunks[--h]);
      _hashChunks[h] = nullptr;
    }
    _hashSingle = true;
#if defined(ESP32)
    // Hashes werden nur 32-bit-weise gelesen/geschrieben und passen daher
    // auch in den IRAM-Heap, der für normales malloc nicht nutzbar ist
    _hashChunks[0] = (uint32_t *)heap_caps_malloc(MAX_SEGS * sizeof(uint32_t),
                                                  MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
#endif
    if (!_hashChunks[0]) _hashChunks[0] = (uint32_t *)malloc(MAX_SEGS * sizeof(uint32_t));
    if (!_hashChunks[0]) {
      freeBuffers();
      return false;
    }
  }

  if (sizeof(T) == 1) {
    _lut = (uint16_t *)malloc(256 * sizeof(uint16_t));
    if (!_lut) {
      freeBuffers();
      return false;
    }
    useRGB332();
  }

  setRotation(_tft.getRotation());
  fillScreen(0);
  return true;
}

template <typename T>
void ILI9341_FramebufferT<T>::mapRows() {
  uint32_t rowsPerChunk = _chunkPx / _width;
  for (int16_t y = 0; y < _height; y++)
    _rows[y] = _chunks[y / rowsPerChunk] + (y % rowsPerChunk) * _width;
}

template <typename T>
void ILI9341_FramebufferT<T>::setRotation(uint8_t r) {
  Adafruit_GFX::setRotation(r);
  _tft.setRotation(r);
  if (_nChunks) mapRows();
  // Scrollen zurücksetzen (Display und Puffer stimmen danach 1:1 überein)
  _tfa = _bfa = _vsp = _vspHw = 0;
  _vsa = ILI9341_TFTHEIGHT;
  _tft.setScrollMargins(0, 0);
  _tft.scrollTo(0);
  _forceAll = true;
}

template <typename T> void ILI9341_FramebufferT<T>::setScrollArea(int16_t fixedStart, int16_t fixedEnd) {
  const int16_t N = ILI9341_TFTHEIGHT;
  fixedStart = constrain(fixedStart, 0, N);
  fixedEnd = constrain(fixedEnd, 0, N - fixedStart);
  bool wasScrolled = scrolled();
  // Bei Rotation 2/3 läuft die Scrollachse gegen die Panel-Zeilen
  _tfa = rotation >= 2 ? fixedEnd : fixedStart;
  _bfa = rotation >= 2 ? fixedStart : fixedEnd;
  _vsa = N - _tfa - _bfa;
  _vsp = _vspHw = _tfa;
  _tft.setScrollMargins(_tfa, _bfa);
  _tft.scrollTo(_vsp);
  if (wasScrolled) _forceAll = true; // Zuordnung Display <-> RAM hat sich geändert
}

template <typename T> void ILI9341_FramebufferT<T>::scroll(int16_t delta) {
  if (!_nChunks || _vsa == 0) return;
  delta %= (int16_t)_vsa;
  if (!delta) return;
  display(); // offene Änderungen zuerst übertragen

  // Inhalt im RAM genauso zyklisch verschieben wie das Display
  const int16_t N = ILI9341_TFTHEIGHT;
  int16_t s0 = rotation >= 2 ? _bfa : _tfa; // logischer Beginn des Scrollbereichs
  int16_t len = _vsa;
  int16_t k = ((delta % len) + len) % len;
  if (!(rotation & 1)) {
    std::rotate(_rows + s0, _rows + s0 + k, _rows + s0 + len);
  } else {
    for (int16_t y = 0; y < _height; y++) std::rotate(_rows[y] + s0, _rows[y] + s0 + k, _rows[y] + s0 + len);
  }
  (void)N;

  int16_t gd = rotation >= 2 ? -delta : delta; // in Gate-Leitungen
  int16_t ofs = (int16_t)(_vsp - _tfa) + gd;
  ofs = ((ofs % len) + len) % len;
  _vsp = _tfa + ofs;
#if defined(ESP32)
  if (!_vsync) // im VSync-Modus schaltet das nächste display() tearing-frei um
#endif
  {
    _tft.scrollTo(_vsp);
    _vspHw = _vsp;
  }
  rebaseline();
}

// Hashes an den aktuellen Pufferinhalt angleichen, ohne zu senden
// (nach scroll() zeigt das Display genau diesen Inhalt)
template <typename T> void ILI9341_FramebufferT<T>::rebaseline() {
  bool force = _forceAll;
  _forceAll = true; // alle Segmente neu hashen, nicht nur beschriebene
  for (int16_t y = 0; y < _height; y++) updateRow(y);
  _forceAll = force;
}

// Rechteck aus dem Puffer senden; bei gescrolltem Display an der Umlaufstelle
// und an festen Bereichen geteilt, mit übersetzten Adressen
template <typename T> void ILI9341_FramebufferT<T>::sendRect(int16_t x0, int16_t y0, int16_t w, int16_t h) {
  if (!scrolled()) {
    _tft.setAddrWindow(x0, y0, w, h);
    for (int16_t r = y0; r < y0 + h; r++) emitPx(_tft, _rows[r] + x0, w, _lut);
    return;
  }
  if (!(rotation & 1)) { // Scrollachse y
    int16_t y = y0;
    while (y < y0 + h) {
      int16_t e = y;
      while (e + 1 < y0 + h && memLogical(e + 1) == memLogical(e) + 1) e++;
      _tft.setAddrWindow(x0, memLogical(y), w, e - y + 1);
      for (int16_t r = y; r <= e; r++) emitPx(_tft, _rows[r] + x0, w, _lut);
      y = e + 1;
    }
  } else { // Scrollachse x
    int16_t x = x0;
    while (x < x0 + w) {
      int16_t e = x;
      while (e + 1 < x0 + w && memLogical(e + 1) == memLogical(e) + 1) e++;
      _tft.setAddrWindow(memLogical(x), y0, e - x + 1, h);
      for (int16_t r = y0; r < y0 + h; r++) emitPx(_tft, _rows[r] + x, e - x + 1, _lut);
      x = e + 1;
    }
  }
}

#if defined(ESP32)
template <typename T> void ILI9341_FramebufferT<T>::screenshot(Print &out) {
  uint16_t line[ILI9341_TFTHEIGHT];
  out.printf("SHOT %d %d\n", _width, _height);
  for (int16_t y = 0; y < _height; y++) {
    readDisplayRow(y, line);
    for (int16_t x = 0; x < _width; x++) line[x] = (line[x] << 8) | (line[x] >> 8);
    out.write((const uint8_t *)line, _width * 2);
  }
  out.println();
}

template <typename T> void ILI9341_FramebufferT<T>::readDisplayRow(int16_t y, uint16_t *out) {
  if (!(rotation & 1)) {
    _tft.readRect(0, memLogical(y), _width, 1, out);
    return;
  }
  int16_t x = 0;
  while (x < _width) {
    int16_t e = x;
    while (e + 1 < _width && memLogical(e + 1) == memLogical(e) + 1) e++;
    _tft.readRect(memLogical(x), y, e - x + 1, 1, out + x);
    x = e + 1;
  }
}
#endif

template <typename T>
void ILI9341_FramebufferT<T>::invertDisplay(bool invert) { _tft.invertDisplay(invert); }

// ---------------------------------------------------------------------------
// Zeichnen in den RAM

template <typename T>
void ILI9341_FramebufferT<T>::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  _rows[y][x] = encodePx(color, _indexed, (T *)nullptr);
  _touched[y] |= 1u << (x / SEG_PX);
}

template <typename T>
void ILI9341_FramebufferT<T>::writePixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  _rows[y][x] = encodePx(color, _indexed, (T *)nullptr);
  _touched[y] |= 1u << (x / SEG_PX);
}

template <typename T>
inline void ILI9341_FramebufferT<T>::fillSpan(int16_t x, int16_t y, int16_t w, int16_t h, T c) {
  if (w < 0) { x += w + 1; w = -w; }
  if (h < 0) { y += h + 1; h = -h; }
  int32_t x1 = (int32_t)x + w, y1 = (int32_t)y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > _width) x1 = _width;
  if (y1 > _height) y1 = _height;
  if (x >= x1 || y >= y1) return;
  int32_t n = x1 - x;
  uint32_t m = segMask(x, x1 - 1);
  if (n == 1) { // senkrechte Linie: direkt schreiben
    for (int16_t yy = y; yy < y1; yy++) {
      _rows[yy][x] = c;
      _touched[yy] |= m;
    }
    return;
  }
  for (int16_t yy = y; yy < y1; yy++) {
    fillPx(_rows[yy] + x, c, n);
    _touched[yy] |= m;
  }
}

template <typename T>
inline void ILI9341_FramebufferT<T>::hspan(int16_t x0, int16_t x1, int16_t y, T c) {
  if ((uint16_t)y >= (uint16_t)_height) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= _width) x1 = _width - 1;
  if (x0 > x1) return;
  fillPx(_rows[y] + x0, c, x1 - x0 + 1);
  _touched[y] |= segMask(x0, x1);
}

// Gleicher Algorithmus wie Adafruit_GFX::fillTriangle, aber ohne virtuellen
// Aufruf je Zeile
template <typename T>
void ILI9341_FramebufferT<T>::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                           int16_t x2, int16_t y2, uint16_t color) {
  T c = encodePx(color, _indexed, (T *)nullptr);
  int16_t a, b, y, last;
  if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
  if (y1 > y2) { std::swap(y2, y1); std::swap(x2, x1); }
  if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
  if (y0 == y2) {
    a = b = x0;
    if (x1 < a) a = x1;
    else if (x1 > b) b = x1;
    if (x2 < a) a = x2;
    else if (x2 > b) b = x2;
    hspan(a, b, y0, c);
    return;
  }
  int16_t dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0, dx12 = x2 - x1,
          dy12 = y2 - y1;
  int32_t sa = 0, sb = 0;
  last = y1 == y2 ? y1 : y1 - 1;
  for (y = y0; y <= last; y++) {
    a = x0 + sa / dy01;
    b = x0 + sb / dy02;
    sa += dx01;
    sb += dx02;
    if (a > b) std::swap(a, b);
    hspan(a, b, y, c);
  }
  sa = (int32_t)dx12 * (y - y1);
  sb = (int32_t)dx02 * (y - y0);
  for (; y <= y2; y++) {
    a = x1 + sa / dy12;
    b = x0 + sb / dy02;
    sa += dx12;
    sb += dx02;
    if (a > b) std::swap(a, b);
    hspan(a, b, y, c);
  }
}

// Gleiche Pixel wie Adafruit_GFX::fillCircle (senkrechte Linien), aber als
// Ausdehnung je Zeile berechnet und waagerecht gefüllt
template <typename T>
void ILI9341_FramebufferT<T>::fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  T c = encodePx(color, _indexed, (T *)nullptr);
  int16_t lo[ILI9341_TFTHEIGHT], hi[ILI9341_TFTHEIGHT];
  const int16_t H = _height;
  int16_t yMin = H, yMax = -1;
  auto vline = [&](int16_t x, int16_t ya, int16_t len) {
    int16_t yb = ya + len - 1;
    if (ya < 0) ya = 0;
    if (yb >= H) yb = H - 1;
    for (int16_t yy = ya; yy <= yb; yy++) {
      if (yy < yMin || yy > yMax) { // Zeile erstmals berührt
        if (yy < yMin) {
          for (int16_t k = yy; k < (yMin < H ? yMin : yy + 1); k++) lo[k] = 32767, hi[k] = -32768;
          yMin = yy;
        }
        if (yy > yMax) {
          for (int16_t k = (yMax >= 0 ? yMax + 1 : yy); k <= yy; k++) lo[k] = 32767, hi[k] = -32768;
          yMax = yy;
        }
      }
      if (x < lo[yy]) lo[yy] = x;
      if (x > hi[yy]) hi[yy] = x;
    }
  };
  vline(x0, y0 - r, 2 * r + 1);
  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r, px = x, py = y;
  const int16_t delta = 1;
  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }
    x++;
    ddF_x += 2;
    f += ddF_x;
    if (x < (y + 1)) {
      vline(x0 + x, y0 - y, 2 * y + delta);
      vline(x0 - x, y0 - y, 2 * y + delta);
    }
    if (y != py) {
      vline(x0 + py, y0 - px, 2 * px + delta);
      vline(x0 - py, y0 - px, 2 * px + delta);
      py = y;
    }
    px = x;
  }
  for (int16_t yy = yMin; yy <= yMax; yy++)
    if (lo[yy] <= hi[yy]) hspan(lo[yy], hi[yy], yy, c);
}

template <typename T>
void ILI9341_FramebufferT<T>::writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                                        uint16_t color) {
  fillSpan(x, y, w, h, encodePx(color, _indexed, (T *)nullptr));
}

template <typename T> void ILI9341_FramebufferT<T>::markDirty(int16_t x, int16_t y, int16_t w, int16_t h) {
  int32_t x1 = (int32_t)x + w, y1 = (int32_t)y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > _width) x1 = _width;
  if (y1 > _height) y1 = _height;
  if (x >= x1 || y >= y1) return;
  uint32_t m = segMask(x, x1 - 1);
  for (int16_t yy = y; yy < y1; yy++) _touched[yy] |= m;
}

// Bresenham direkt in den Puffer (statt eines virtuellen writePixel je Punkt)
template <typename T>
void ILI9341_FramebufferT<T>::writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  T c = encodePx(color, _indexed, (T *)nullptr);
  if (y0 == y1) {
    if (x1 < x0) std::swap(x0, x1);
    fillSpan(x0, y0, x1 - x0 + 1, 1, c);
    return;
  }
  if (x0 == x1) {
    if (y1 < y0) std::swap(y0, y1);
    fillSpan(x0, y0, 1, y1 - y0 + 1, c);
    return;
  }
  bool steep = abs(y1 - y0) > abs(x1 - x0);
  if (steep) {
    std::swap(x0, y0);
    std::swap(x1, y1);
  }
  if (x0 > x1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  // Wie Adafruit_GFX::writeLine, aber vorab auf das Bild zugeschnitten: Der
  // Bresenham-Zustand nach n Schritten ist exakt berechenbar, also springen
  // wir zum ersten sichtbaren Punkt und hören nach dem letzten auf.
  const int32_t dx = x1 - x0, dy = abs(y1 - y0), half = dx / 2;
  const int16_t ystep = y0 < y1 ? 1 : -1;
  const int32_t majN = steep ? _height : _width, minN = steep ? _width : _height;
  auto ySteps = [&](int32_t n) -> int32_t { // Nebenachsen-Schritte vor Punkt n
    int32_t num = n * dy - half;
    return num > 0 ? (num + dx - 1) / dx : 0;
  };
  auto minorAt = [&](int32_t n) -> int32_t { return y0 + ystep * ySteps(n); };
  int32_t nA = max<int32_t>(0, -x0), nB = min<int32_t>(dx, majN - 1 - x0);
  if (nA > nB) return;
  // Nebenachse ist monoton: ersten und letzten Punkt im Bild per Binärsuche
  auto inLow = [&](int32_t n) { int32_t m = minorAt(n); return ystep > 0 ? m >= 0 : m < minN; };
  auto inHigh = [&](int32_t n) { int32_t m = minorAt(n); return ystep > 0 ? m < minN : m >= 0; };
  int32_t lo = nA, hi = nB + 1;
  while (lo < hi) { // erstes n mit inLow
    int32_t mid = (lo + hi) / 2;
    if (inLow(mid)) hi = mid;
    else lo = mid + 1;
  }
  nA = lo;
  lo = nA - 1, hi = nB;
  while (lo < hi) { // letztes n mit inHigh
    int32_t mid = (lo + hi + 1) / 2;
    if (inHigh(mid)) lo = mid;
    else hi = mid - 1;
  }
  nB = lo;
  if (nA > nB) return;

  int32_t k = ySteps(nA);
  int32_t err = half - nA * dy + k * dx;
  int16_t yy = y0 + ystep * k;
  for (int32_t n = nA; n <= nB; n++) {
    int16_t xx = x0 + n;
    int16_t px = steep ? yy : xx, py = steep ? xx : yy;
    _rows[py][px] = c;
    _touched[py] |= 1u << (px / SEG_PX);
    err -= dy;
    if (err < 0) {
      yy += ystep;
      err += dx;
    }
  }
}

template <typename T>
void ILI9341_FramebufferT<T>::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  writeLine(x0, y0, x1, y1, color);
}

// Ein Zeichen mit Läufen statt Einzelpixeln; Ergebnis identisch zu Adafruit_GFX::drawChar
template <typename T>
void ILI9341_FramebufferT<T>::fastChar(int16_t x, int16_t y, unsigned char ch, uint16_t color,
                                       uint16_t bg, uint8_t sx, uint8_t sy) {
  T fg = encodePx(color, _indexed, (T *)nullptr);
  if (!gfxFont) {
    if (x >= _width || y >= _height || x + 6 * sx - 1 < 0 || y + 8 * sy - 1 < 0) return;
    if (!_cp437 && ch >= 176) ch++;
    if (bg != color) fillSpan(x, y, 6 * sx, 8 * sy, encodePx(bg, _indexed, (T *)nullptr));
    const uint8_t *g = &font[ch * 5];
    uint8_t col[5];
    for (int i = 0; i < 5; i++) col[i] = pgm_read_byte(&g[i]);
    for (int j = 0; j < 8; j++) {
      int i = 0;
      while (i < 5) {
        if (!((col[i] >> j) & 1)) {
          i++;
          continue;
        }
        int s = i;
        while (i < 5 && ((col[i] >> j) & 1)) i++;
        fillSpan(x + s * sx, y + j * sy, (i - s) * sx, sy, fg);
      }
    }
    return;
  }

  // Eigener Font: Bitmap zeilenweise, Bits fortlaufend
  const GFXglyph *glyph = &gfxFont->glyph[ch - (uint8_t)pgm_read_byte(&gfxFont->first)];
  const uint8_t *bitmap = gfxFont->bitmap + pgm_read_word(&glyph->bitmapOffset);
  uint8_t w = pgm_read_byte(&glyph->width), h = pgm_read_byte(&glyph->height);
  int16_t xo = (int8_t)pgm_read_byte(&glyph->xOffset), yo = (int8_t)pgm_read_byte(&glyph->yOffset);
  uint8_t bits = 0, bit = 0;
  for (uint8_t yy = 0; yy < h; yy++) {
    int16_t run = -1;
    for (uint8_t xx = 0; xx < w; xx++) {
      if (!(bit++ & 7)) bits = pgm_read_byte(bitmap++);
      bool on = bits & 0x80;
      bits <<= 1;
      if (on && run < 0) run = xx;
      if (!on && run >= 0) {
        fillSpan(x + (xo + run) * sx, y + (yo + yy) * sy, (xx - run) * sx, sy, fg);
        run = -1;
      }
    }
    if (run >= 0) fillSpan(x + (xo + run) * sx, y + (yo + yy) * sy, (w - run) * sx, sy, fg);
  }
}

// Gleiche Cursor-Logik wie Adafruit_GFX::write(), aber mit fastChar()
template <typename T> size_t ILI9341_FramebufferT<T>::write(uint8_t c) {
  if (!gfxFont) {
    if (c == '\n') {
      cursor_x = 0;
      cursor_y += textsize_y * 8;
    } else if (c != '\r') {
      if (wrap && (cursor_x + textsize_x * 6) > _width) {
        cursor_x = 0;
        cursor_y += textsize_y * 8;
      }
      fastChar(cursor_x, cursor_y, c, textcolor, textbgcolor, textsize_x, textsize_y);
      cursor_x += textsize_x * 6;
    }
    return 1;
  }
  if (c == '\n') {
    cursor_x = 0;
    cursor_y += (int16_t)textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
  } else if (c != '\r') {
    uint8_t first = pgm_read_byte(&gfxFont->first);
    if (c >= first && c <= (uint8_t)pgm_read_byte(&gfxFont->last)) {
      const GFXglyph *glyph = &gfxFont->glyph[c - first];
      uint8_t w = pgm_read_byte(&glyph->width), h = pgm_read_byte(&glyph->height);
      if (w > 0 && h > 0) {
        int16_t xo = (int8_t)pgm_read_byte(&glyph->xOffset);
        if (wrap && (cursor_x + textsize_x * (xo + w)) > _width) {
          cursor_x = 0;
          cursor_y += (int16_t)textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
        }
        fastChar(cursor_x, cursor_y, c, textcolor, textbgcolor, textsize_x, textsize_y);
      }
      cursor_x += (uint8_t)pgm_read_byte(&glyph->xAdvance) * (int16_t)textsize_x;
    }
  }
  return 1;
}

template <typename T>
void ILI9341_FramebufferT<T>::writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  writeFillRect(x, y, w, 1, color);
}

template <typename T>
void ILI9341_FramebufferT<T>::writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  writeFillRect(x, y, 1, h, color);
}

template <typename T>
void ILI9341_FramebufferT<T>::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  writeFillRect(x, y, w, h, color);
}

template <typename T>
void ILI9341_FramebufferT<T>::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  writeFillRect(x, y, w, 1, color);
}

template <typename T>
void ILI9341_FramebufferT<T>::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  writeFillRect(x, y, 1, h, color);
}

template <typename T>
void ILI9341_FramebufferT<T>::fillScreen(uint16_t color) {
  writeFillRect(0, 0, _width, _height, color);
}

template <typename T>
void ILI9341_FramebufferT<T>::pushImage(int16_t x, int16_t y, int16_t w, int16_t h,
                                    const uint16_t *data) {
  if (w <= 0 || h <= 0) return;
  int16_t sx = 0, sy = 0, cw = w, ch = h;
  if (x < 0) { sx = -x; cw += x; x = 0; }
  if (y < 0) { sy = -y; ch += y; y = 0; }
  if (x + cw > _width) cw = _width - x;
  if (y + ch > _height) ch = _height - y;
  if (cw <= 0 || ch <= 0) return;
  const uint16_t *src = data + (int32_t)sy * w + sx;
  uint32_t m = segMask(x, x + cw - 1);
  for (int16_t r = 0; r < ch; r++, src += w) {
    _touched[y + r] |= m;
    if (sizeof(T) == 2) {
      memcpy(_rows[y + r] + x, src, cw * 2);
    } else {
      T *dst = _rows[y + r] + x;
      for (int16_t i = 0; i < cw; i++) dst[i] = encodePx(src[i], false, (T *)nullptr);
    }
  }
}

template <typename T>
uint16_t ILI9341_FramebufferT<T>::getPixel(int16_t x, int16_t y) const {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return 0;
  uint16_t c = swappedPx(_rows[y][x], _lut);
  return (c << 8) | (c >> 8);
}

// ---------------------------------------------------------------------------
// Übertragung

template <typename T>
void ILI9341_FramebufferT<T>::updateRow(int16_t y) {
  const int16_t W = _width;
  const int segs = (W + SEG_PX - 1) / SEG_PX;
  const T *row = _rows[y];
  uint32_t *hs = hashRow(y, segs);
  uint32_t mask = 0;
  const bool force = _forceAll;
  // Nur seit dem letzten display() beschriebene Segmente können sich geändert haben
  const uint32_t touched = force ? ~0u : _touched[y];
  _touched[y] = 0;
  if (!touched) {
    _rowMask[y] = 0;
    return;
  }
  const int full = W / SEG_PX; // volle Segmente: feste Länge, vom Compiler ausgerollt
  const uint32_t *q = (const uint32_t *)row;
  const int words = SEG_PX * sizeof(T) / 4;
  int s = 0;
  // Zwei Segmente verschränkt: zwei unabhängige Multiplikationsketten, die
  // der Prozessor überlappen kann (jedes Segment behält seinen eigenen Hash)
  for (; s + 1 < full; s += 2, q += 2 * words) {
    if (((touched >> s) & 3) != 3) {
      for (int k = 0; k < 2; k++) {
        if (!((touched >> (s + k)) & 1)) continue;
        const uint32_t *qk = q + k * words;
        uint32_t h = 0x811C9DC5;
        for (int i = 0; i < words; i++) h = (h ^ qk[i]) * 0x01000193;
        if (force || h != hs[s + k]) {
          hs[s + k] = h;
          mask |= 1UL << (s + k);
        }
      }
      continue;
    }
    uint32_t h0 = 0x811C9DC5, h1 = 0x811C9DC5;
    const uint32_t *q1 = q + words;
    for (int i = 0; i < words; i++) {
      h0 = (h0 ^ q[i]) * 0x01000193;
      h1 = (h1 ^ q1[i]) * 0x01000193;
    }
    if (force || h0 != hs[s]) {
      hs[s] = h0;
      mask |= 1UL << s;
    }
    if (force || h1 != hs[s + 1]) {
      hs[s + 1] = h1;
      mask |= 1UL << (s + 1);
    }
  }
  for (; s < full; s++, q += words) {
    if (!((touched >> s) & 1)) continue;
    uint32_t h = 0x811C9DC5;
    for (int i = 0; i < words; i++) h = (h ^ q[i]) * 0x01000193;
    if (force || h != hs[s]) {
      hs[s] = h;
      mask |= 1UL << s;
    }
  }
  if (full < segs && ((touched >> full) & 1)) {
    // Restsegment (z.B. 16 px bei 240 px Breite): wortweise, wenn möglich
    const int bytes = (W - full * SEG_PX) * sizeof(T);
    uint32_t h = 0x811C9DC5;
    if (!(bytes & 3)) {
      for (int i = 0; i < bytes / 4; i++) h = (h ^ q[i]) * 0x01000193;
    } else {
      h = hashBytes((const uint8_t *)(row + full * SEG_PX), bytes);
    }
    if (_forceAll || h != hs[full]) {
      hs[full] = h;
      mask |= 1UL << full;
    }
  }
  _rowMask[y] = mask;
}

// Zerlegt eine Segmentmaske in zusammenhängende Läufe. Lücken werden
// mitgeschickt, wenn das bei Höhe h billiger ist als ein weiteres Fenster.
template <typename T>
uint8_t ILI9341_FramebufferT<T>::buildRuns(uint32_t mask, int32_t h, Run *runs) const {
  uint8_t n = 0;
  int s = 0;
  while (mask >> s) {
    while (!((mask >> s) & 1)) s++;
    int e = s;
    while ((mask >> (e + 1)) & 1) e++;
    if (n && (s - runs[n - 1].s1 - 1) * SEG_PX * h <= _windowCost) {
      runs[n - 1].s1 = e; // Lücke schließen
    } else {
      runs[n].s0 = s;
      runs[n].s1 = e;
      n++;
    }
    s = e + 1;
    if (s >= 32) break;
  }
  return n;
}

// Kosten in Pixel-Äquivalenten: übertragene Pixel plus Fensterkosten
template <typename T>
int32_t ILI9341_FramebufferT<T>::cost(uint32_t mask, int32_t h) const {
  Run runs[16];
  uint8_t n = buildRuns(mask, h, runs);
  int32_t c = n * _windowCost;
  for (uint8_t i = 0; i < n; i++) {
    int32_t x1 = (runs[i].s1 + 1) * SEG_PX;
    c += ((x1 > _width ? _width : x1) - runs[i].s0 * SEG_PX) * h;
  }
  return c;
}

template <typename T>
void ILI9341_FramebufferT<T>::display() {
  if (!_nChunks) return;
#if defined(ESP32)
  if (_vsync) {
    displayVSync();
    return;
  }
#endif

  const int16_t W = _width, H = _height;
  const int segs = (W + SEG_PX - 1) / SEG_PX;
  int16_t hashed = 0; // Zeilen werden erst gehasht, wenn sie gebraucht werden
  auto need = [&](int16_t r) {
    while (hashed <= r) updateRow(hashed++);
  };
  auto isFullWidth = [&](uint32_t mask, int32_t h) {
    Run runs[16];
    return buildRuns(mask, h, runs) == 1 && runs[0].s0 == 0 && runs[0].s1 == segs - 1;
  };

  // Zeilen gierig zu Gruppen zusammenfassen, solange das billiger ist als
  // getrennte Fenster. Eine Gruppe besteht aus einem oder mehreren Rechtecken
  // (Läufen geänderter Segmente). Deckt sie die volle Breite ab, kann sie
  // nicht mehr wachsen - dann wird sofort gestreamt, damit Hashen und
  // DMA-Übertragung parallel laufen.
  uint32_t sent = 0;
  _tft.startWrite();
  int16_t y = 0;
  while (y < H) {
    need(y);
    uint32_t m = _rowMask[y];
    if (!m) {
      y++;
      continue;
    }
    int16_t ys = y, ye = y;
    bool streaming = false;
    auto startStreaming = [&]() {
      _tft.setAddrWindow(0, ys, W, H - ys);
      for (int16_t k = ys; k <= ye; k++) emitPx(_tft, _rows[k], W, _lut);
      streaming = true;
    };
    const bool canStream = !scrolled(); // Streaming-Fenster geht bis zum Rand
    if (canStream && isFullWidth(m, 1)) startStreaming();

    for (int16_t r = ye + 1; r < H; r++) {
      need(r);
      uint32_t mr = _rowMask[r];
      if (!mr) {
        // Lücke: lohnt sich nur, wenn sie billiger als ein neues Fenster ist
        if ((r - ye) * SEG_PX > _windowCost) break;
        continue;
      }
      int32_t merged = cost(m | mr, r - ys + 1);
      int32_t separate = cost(m, ye - ys + 1) + cost(mr, 1);
      if (merged > separate) break;
      m |= mr;
      if (streaming) {
        for (int16_t k = ye + 1; k <= r; k++) emitPx(_tft, _rows[k], W, _lut);
        ye = r;
      } else {
        ye = r;
        if (canStream && isFullWidth(m, ye - ys + 1)) startStreaming();
      }
    }

    int16_t h = ye - ys + 1;
    if (streaming) {
      sent += (uint32_t)W * h;
    } else {
      Run runs[16];
      uint8_t n = buildRuns(m, h, runs);
      for (uint8_t i = 0; i < n; i++) {
        int16_t x0 = runs[i].s0 * SEG_PX;
        int16_t x1 = (runs[i].s1 + 1) * SEG_PX;
        if (x1 > W) x1 = W;
        int16_t w = x1 - x0;
        sendRect(x0, ys, w, h);
        sent += (uint32_t)w * h;
      }
    }
    y = ye + 1;
  }
  _tft.endWrite();

  _forceAll = false;
  _lastSent = sent;
}

#if defined(ESP32)
// ===========================================================================
// VSync-Modus
//
// Übertragen wird in nativer Adressierung (MADCTL 0x08): v = Speicherzeile =
// Gate-Leitung, die der Bildaufbau des Panels nacheinander abfährt; u = Spalte.
// Jede Rotation bildet ihre logischen Koordinaten (x, y) darauf ab:
//   0: v = y,       u = W-1-x      2: v = H-1-y,   u = x
//   1: v = x,       u = y          3: v = W-1-x,   u = H-1-y
// ===========================================================================

static const int16_t BAND_LINES = 32; // Streifenhöhe in Gate-Leitungen
static const float ACTIVE_OFS = 2;    // erste sichtbare Zeile im Scanline-Zähler (VBP)
static const float MARGIN = 4;        // Sicherheitsabstand zum Bildaufbau in Zeilen
static const float WINDOW_US = 15;    // gemessene Kosten eines Adressfensters
static const float START_LINES = 2;   // Vorlauf bis zum ersten Pixel (in Zeilen)
static const uint16_t MAX_PLAN_BANDS = 160;
// Bildwiederholraten für VSync (FRMCTR1): ~92 Hz, ~55 Hz, ~48 Hz
static const uint8_t RATE_DIVA[] = {0, 1, 1};
static const uint8_t RATE_RTNA[] = {0x18, 0x14, 0x17};

template <typename T>
bool ILI9341_FramebufferT<T>::setVSync(bool enable) {
  _vsync = enable;
  _late = 0;
  if (!enable) {
    if (_vspHw != _vsp) {
      _tft.scrollTo(_vsp);
      _vspHw = _vsp;
    }
    _tft.setFrameRate(0, 0x18); // Standard der Init-Sequenz (~92 Hz)
    return true;
  }
  // Alle Bildwiederholraten kalibrieren, display() wählt je Frame
  for (uint8_t i = 0; i < VSYNC_RATES; i++) {
    _tft.setFrameRate(RATE_DIVA[i], RATE_RTNA[i]);
    _rateLineUs[i] = calibrateLineUs();
    if (_rateLineUs[i] <= 0) {
      _vsync = false; // kein MISO / keine Antwort
      _tft.setFrameRate(0, 0x18);
      return false;
    }
  }
  _tft.setFrameRate(RATE_DIVA[VSYNC_RATES - 1], RATE_RTNA[VSYNC_RATES - 1]);
  _rate = VSYNC_RATES - 1;
  _lineUs = _rateLineUs[_rate];
  _forceAll = true;
  return true;
}

template <typename T> float ILI9341_FramebufferT<T>::calibrateLineUs() {
  delay(40);
  // Zeilenzahl: ein ganzes Bild lang ohne Pause abtasten (ein Lesevorgang
  // dauert nur wenige µs, so wird auch die kurze Austastlücke sicher erfasst)
  uint16_t maxLine = 0;
  uint32_t ts = millis();
  while (millis() - ts < 40) {
    uint16_t l = _tft.getScanline();
    if (l > maxLine) maxLine = l;
  }
  _lines = maxLine + 1;

  // Zeilenzeit über eine lange Strecke messen
  const int N = 150;
  uint32_t t0 = micros(), adv = 0;
  uint16_t samples[N];
  samples[0] = _tft.getScanline();
  for (int i = 1; i < N; i++) {
    delayMicroseconds(1200);
    samples[i] = _tft.getScanline();
  }
  uint32_t t1 = micros();
  for (int i = 1; i < N; i++)
    adv += samples[i] >= samples[i - 1] ? samples[i] - samples[i - 1]
                                         : samples[i] + _lines - samples[i - 1];
  return adv ? (float)(t1 - t0) / adv : 0;
}

// Liefert die Übertragungsstreifen in aufsteigender Gate-Reihenfolge:
// fn(v0, v1, runs, nRuns), runs in u-Koordinaten.
template <typename T>
template <typename F>
void ILI9341_FramebufferT<T>::forEachBand(F fn) {
  const int16_t W = _width, H = _height;
  const int segs = (W + SEG_PX - 1) / SEG_PX;
  const uint8_t r = getRotation();
  RunU ru[33];

  if (!(r & 1)) {
    // Hochformat: Gate-Leitung = Bildzeile. Zeilen gierig gruppieren wie in
    // display(), dann in Streifen von höchstens BAND_LINES aufteilen.
    int16_t v = 0;
    while (v < H) {
      int16_t y = r == 0 ? v : H - 1 - v;
      uint32_t m = _rowMask[y];
      if (!m) {
        v++;
        continue;
      }
      int16_t vs = v, ve = v;
      for (int16_t vr = v + 1; vr < H && vr - vs < BAND_LINES; vr++) {
        uint32_t mr = _rowMask[r == 0 ? vr : H - 1 - vr];
        if (!mr) {
          if ((vr - ve) * SEG_PX > _windowCost) break;
          continue;
        }
        if (cost(m | mr, vr - vs + 1) > cost(m, ve - vs + 1) + cost(mr, 1)) break;
        m |= mr;
        ve = vr;
      }
      Run runs[16];
      uint8_t n = buildRuns(m, ve - vs + 1, runs);
      for (uint8_t i = 0; i < n; i++) {
        int16_t x0 = runs[i].s0 * SEG_PX, x1 = min((runs[i].s1 + 1) * SEG_PX, (int)W) - 1;
        // Reihenfolge der Läufe in u aufsteigend
        uint8_t k = r == 0 ? n - 1 - i : i;
        ru[k].u0 = r == 0 ? W - 1 - x1 : x0;
        ru[k].u1 = r == 0 ? W - 1 - x0 : x1;
      }
      fn(vs, ve, ru, n);
      v = ve + 1;
    }
    return;
  }

  // Querformat: Gate-Leitung = Bildspalte. Je Segmentspalte ein Streifen,
  // darin Läufe geänderter Bildzeilen (Lücken nach Kosten geschlossen).
  for (int k = 0; k < segs; k++) {
    int s = r == 1 ? k : segs - 1 - k;
    int16_t x0 = s * SEG_PX, x1 = min((s + 1) * SEG_PX, (int)W) - 1;
    int16_t v0 = r == 1 ? x0 : W - 1 - x1, v1 = r == 1 ? x1 : W - 1 - x0;
    uint8_t n = 0;
    int16_t y = 0;
    while (y < H) {
      if (!((_rowMask[y] >> s) & 1)) {
        y++;
        continue;
      }
      int16_t y0 = y, y1 = y;
      while (y1 + 1 < H && ((_rowMask[y1 + 1] >> s) & 1)) y1++;
      // Lücke zum vorigen Lauf schließen, wenn billiger als ein neues Fenster
      if (n && (y0 - ru[n - 1].u1 - 1) * (x1 - x0 + 1) <= _windowCost) {
        ru[n - 1].u1 = y1;
      } else if (n < 32) {
        ru[n].u0 = y0;
        ru[n].u1 = y1;
        n++;
      } else {
        ru[n - 1].u1 = y1;
      }
      y = y1 + 1;
    }
    if (!n) continue;
    if (r == 3) { // u = H-1-y: Läufe spiegeln und umdrehen
      for (uint8_t i = 0; i < n; i++) {
        int16_t a = H - 1 - ru[i].u1, b = H - 1 - ru[i].u0;
        ru[i].u0 = a;
        ru[i].u1 = b;
      }
      for (uint8_t i = 0; i < n / 2; i++) {
        RunU t = ru[i];
        ru[i] = ru[n - 1 - i];
        ru[n - 1 - i] = t;
      }
    }
    fn(v0, v1, ru, n);
  }
}

template <typename T>
void ILI9341_FramebufferT<T>::displayVSync() {
  const int16_t W = _width, H = _height;
  const uint8_t r = getRotation();
  for (int16_t y = 0; y < H; y++) updateRow(y);

  // Mit dem Bildaufbau synchronisieren (Lesen geht nur bei freiem Bus)
  _tft.finishWrites();
  const float L = _lines;
  const float usPerPx = 16.0f * 1e6f / _tft.spiSpeed();

  // Steht ein Scroll-Wechsel an, wird der neue Offset in der Austastlücke nach
  // dem aktuellen Refresh umgeschaltet. Jede Speicherzeile darf erst
  // beschrieben werden, wenn der Bildaufbau ihre *alte* Anzeigeposition
  // passiert hat, und muss fertig sein, bevor er im nächsten Refresh ihre
  // *neue* Position erreicht. Dann erscheinen Verschiebung und neuer Inhalt
  // gemeinsam im selben Refresh.
  const bool pend = _vspHw != _vsp;
  auto oldLine = [&](uint16_t m) -> uint16_t { // Speicherzeile -> heutige Anzeigezeile
    if (m < _tfa || m >= _tfa + _vsa) return m;
    return _tfa + ((m - _tfa) - (_vspHw - _tfa) + _vsa) % _vsa;
  };
  auto passLine = [&](int16_t v0, int16_t v1) { // letzte Anzeigezeile, die passiert sein muss
    int16_t mx = 0;
    for (int16_t v = v0; v <= v1; v++) mx = max(mx, (int16_t)oldLine(memLine(v)));
    return mx;
  };
  auto deadline = [&](float k0, int16_t v0) { return k0 + L + v0 + ACTIVE_OFS - MARGIN; };

  // Streifen einmal erfassen (Position, zu passierende Zeile, Dauer); alle
  // Simulationen laufen danach nur über diese Liste
  struct BandInfo {
    int16_t v0, pass;
    float us;
  };
  static BandInfo bands[MAX_PLAN_BANDS];
  uint16_t nBands = 0;
  bool overflow = false;
  forEachBand([&](int16_t v0, int16_t v1, const RunU *ru, uint8_t n) {
    if (nBands >= MAX_PLAN_BANDS) {
      overflow = true;
      return;
    }
    uint32_t px = 0;
    for (uint8_t i = 0; i < n; i++) px += (ru[i].u1 - ru[i].u0 + 1) * (v1 - v0 + 1);
    bands[nBands++] = {v0, passLine(v0, v1), px * usPerPx + n * WINDOW_US};
  });

  // Plan simulieren: gestartet bei Zählerstand cs (Refresh beginnt bei k0),
  // Zeilenzeit lt, Übertragungsdauern mit Sicherheitsfaktor k. Wird jeder
  // Streifen fertig, bevor der nächste Refresh ihn erreicht?
  auto feasible = [&](float cs, float k0, float lt, float k) {
    if (overflow) return false; // zu viele Streifen: sicherheitshalber langsam/oben beginnen
    float t = 0;
    for (uint16_t i = 0; i < nBands; i++) {
      float thr = k0 + bands[i].pass + ACTIVE_OFS + MARGIN;
      t = max(t, (thr - cs) * lt) + bands[i].us * k;
      if (t > (deadline(k0, bands[i].v0) - cs) * lt) return false;
    }
    return true;
  };

  // Bildwiederholrate wählen: die schnellste, bei der der Frame - am oberen
  // Rand begonnen - sicher hinter dem Bildaufbau her passt. Für einen Wechsel
  // zu einer schnelleren Rate wird mehr Reserve verlangt (Hysterese).
  uint8_t rate = VSYNC_RATES - 1;
  for (uint8_t i = 0; i < VSYNC_RATES; i++) {
    float k = i < _rate ? 1.35f : 1.2f;
    if (feasible(ACTIVE_OFS - MARGIN, 0, _rateLineUs[i], k)) {
      rate = i;
      break;
    }
  }
  if (rate != _rate) {
    _tft.setFrameRate(RATE_DIVA[rate], RATE_RTNA[rate]);
    _tft.finishWrites();
    _rate = rate;
  }
  _lineUs = _rateLineUs[_rate];
  const float Lt = _lineUs;
  const float c0 = _tft.getScanline();
  const uint32_t t0 = micros();
  auto counter = [&]() { return c0 + (micros() - t0) / Lt; };

  // k0 = Zählerstand, an dem der Refresh beginnt, hinter dem wir herschreiben.
  // Die Position wird unmittelbar vor dem Senden bestimmt; START_LINES deckt
  // die ersten Kommandos (MADCTL, erstes Fenster) ab.
  float cs = counter() + START_LINES;
  float k0 = floorf(cs / L) * L;
  if (!feasible(cs, k0, Lt, 1.2f)) {
    // Erst kurz vor dem oberen Rand des nächsten Bildaufbaus beginnen
    k0 += L;
    float target = k0 + ACTIVE_OFS - MARGIN;
    while (counter() < target) {
    }
  }

  // Scroll-Offset in der Austastlücke umschalten (Bus muss dafür frei sein)
  const float porch = k0 + ILI9341_TFTHEIGHT + ACTIVE_OFS;
  bool vspDone = !pend;
  auto switchScroll = [&]() {
    _tft.endWrite();
    _tft.finishWrites();
    if (counter() > porch + (L - ILI9341_TFTHEIGHT) - 1) _late++; // Lücke verpasst
    while (counter() < porch) {
    }
    _tft.scrollTo(_vsp);
    _vspHw = _vsp;
    vspDone = true;
    _tft.startWrite();
    _tft.setMadctl(ILI9341_MADCTL_BGR);
  };

  uint32_t sent = 0;
  _tft.startWrite();
  _tft.setMadctl(ILI9341_MADCTL_BGR); // native Adressierung
  forEachBand([&](int16_t v0, int16_t v1, const RunU *ru, uint8_t n) {
    // Erst senden, wenn der Bildaufbau den Streifen in diesem Refresh passiert hat
    float thr = k0 + passLine(v0, v1) + ACTIVE_OFS + MARGIN;
    if (!vspDone) {
      // Würde der Streifen in die Austastlücke hineinlaufen? Dann erst umschalten.
      uint32_t px = 0;
      for (uint8_t i = 0; i < n; i++) px += (ru[i].u1 - ru[i].u0 + 1) * (v1 - v0 + 1);
      float dur = (px * usPerPx + n * WINDOW_US + _tft.pendingBytes() * usPerPx / 2) / Lt;
      if (max(counter(), thr) + dur > porch - 1) switchScroll();
    }
    while (counter() < thr) {
    }

    for (uint8_t i = 0; i < n; i++) {
      int16_t u0 = ru[i].u0, w = ru[i].u1 - u0 + 1;
      for (int16_t v = v0; v <= v1; v++) {
        // Fenster je zusammenhängendem Stück Speicherzeilen (Scrollen)
        if (v == v0 || memLine(v) != memLine(v - 1) + 1) {
          int16_t e = v;
          while (e + 1 <= v1 && memLine(e + 1) == memLine(e) + 1) e++;
          _tft.setAddrWindow(u0, memLine(v), w, e - v + 1);
        }
        int16_t done = 0;
        while (done < w) {
          uint32_t cnt;
          uint16_t *dst = _tft.pixelSpace(w - done, cnt);
          int16_t u = u0 + done;
          switch (r) {
          case 0: { // x = W-1-u, y = v: Zeile rückwärts
            const T *src = _rows[v] + (W - 1 - u);
            for (uint32_t j = 0; j < cnt; j++) dst[j] = swappedPx(src[-(int32_t)j], _lut);
            break;
          }
          case 2: { // x = u, y = H-1-v: Zeile vorwärts
            const T *src = _rows[H - 1 - v] + u;
            for (uint32_t j = 0; j < cnt; j++) dst[j] = swappedPx(src[j], _lut);
            break;
          }
          case 1: // x = v, y = u: Spalte abwärts
            for (uint32_t j = 0; j < cnt; j++) dst[j] = swappedPx(_rows[u + j][v], _lut);
            break;
          default: { // x = W-1-v, y = H-1-u: Spalte aufwärts
            const int16_t x = W - 1 - v;
            for (uint32_t j = 0; j < cnt; j++) dst[j] = swappedPx(_rows[H - 1 - u - j][x], _lut);
            break;
          }
          }
          _tft.commitPixels(cnt);
          done += cnt;
        }
      }
      sent += (uint32_t)w * (v1 - v0 + 1);
    }
    // Wann ist dieser Streifen wirklich gesendet? (Warteschlange mitgerechnet)
    if (counter() + _tft.pendingBytes() * usPerPx / 2 / Lt > deadline(k0, v0)) _late++;
  });
  if (!vspDone) switchScroll();
  _tft.setMadctl(_tft.rotationMadctl());
  _tft.endWrite(); // Übertragung läuft im Hintergrund weiter

  _forceAll = false;
  _lastSent = sent;
}
#endif

template class ILI9341_FramebufferT<uint16_t>;
template class ILI9341_FramebufferT<uint8_t>;
