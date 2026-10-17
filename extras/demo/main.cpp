#include <Arduino.h>
#include <esp_heap_caps.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include "ILI9341_GFX.h"
#include "ILI9341_Framebuffer.h"

// Verdrahtung NodeMCU-32S (VSPI): SCK=18, MOSI=23, MISO=19
#define TFT_CS  15
#define TFT_DC  2
#define TFT_RST 4
#define TFT_BL  13  // Hintergrundbeleuchtung

ILI9341_GFX tft(TFT_CS, TFT_DC, TFT_RST);
// Farbtiefe des Framebuffers: 16 (RGB565, 150 KB) oder 8 (RGB332/Palette, 75 KB)
#ifndef FB_BITS
#define FB_BITS 16
#endif
#if FB_BITS == 8
ILI9341_Framebuffer8 fb(tft);
#else
ILI9341_Framebuffer fb(tft);
#endif

static const int16_t W = 320, H = 240;

static inline uint16_t rgb(float r, float g, float b) {
  return ILI9341_GFX::color565(constrain(r, 0, 255), constrain(g, 0, 255), constrain(b, 0, 255));
}

// ===========================================================================
// Szene 1: Plasma - jeder Pixel ändert sich in jedem Frame
// ===========================================================================
namespace plasma {
uint8_t sinT[256];
uint16_t pal[256];
uint8_t colA[W], colB[W];

void init() {
  for (int i = 0; i < 256; i++) {
    float a = i * TWO_PI / 256;
    sinT[i] = 128 + 127 * sinf(a);
    pal[i] = rgb(128 + 127 * sinf(a), 128 + 127 * sinf(a + 2.1f), 128 + 127 * sinf(a + 4.2f));
  }
}

void frame(uint32_t ms) {
  uint8_t a = ms / 23, b = ms / 31, c = ms / 17, d = ms / 41, shift = ms / 12;
  for (int x = 0; x < W; x++) {
    colA[x] = sinT[(uint8_t)(x * 2 + a)];
    colB[x] = sinT[(uint8_t)(x + c)];
  }
  for (int y = 0; y < H; y++) {
    auto *row = fb.getRow(y);
    uint8_t r = sinT[(uint8_t)(y * 3 + b)];
    for (int x = 0; x < W; x++) {
      uint16_t v = colA[x] + r + sinT[(uint8_t)(colB[x] + y)] + sinT[(uint8_t)(x - 2 * y + d)];
#if FB_BITS == 8
      row[x] = (uint8_t)((v >> 2) + shift); // Palettenindex, Palette siehe startScene()
#else
      row[x] = pal[(uint8_t)((v >> 2) + shift)];
#endif
    }
  }
}
} // namespace plasma

// ===========================================================================
// Szene 2: Synthwave mit flach schattiertem 3D-Torus
// ===========================================================================
namespace synth {
const int HOR = 150;     // Horizont
const int U = 20, V = 10; // Torus-Segmente
const float R = 1.0f, r = 0.42f;

struct Vec { float x, y, z; };
struct Face { float depth; uint16_t color; uint16_t idx; };

Vec verts[U * V], normals[U * V];
int16_t sx[U * V], sy[U * V];
Vec view[U * V];
Face faces[U * V];
uint16_t skyCol[HOR];

void init() {
  for (int u = 0; u < U; u++)
    for (int v = 0; v < V; v++) {
      float p = u * TWO_PI / U, t = v * TWO_PI / V;
      verts[u * V + v] = {(R + r * cosf(t)) * cosf(p), (R + r * cosf(t)) * sinf(p), r * sinf(t)};
      float pc = (u + 0.5f) * TWO_PI / U, tc = (v + 0.5f) * TWO_PI / V;
      normals[u * V + v] = {cosf(tc) * cosf(pc), cosf(tc) * sinf(pc), sinf(tc)};
    }
  for (int y = 0; y < HOR; y++) {
    float k = (float)y / HOR;
    if (k < 0.6f) {
      float q = k / 0.6f;
      skyCol[y] = rgb(15 + q * 95, 3 + q * 12, 45 + q * 70);
    } else {
      float q = (k - 0.6f) / 0.4f;
      skyCol[y] = rgb(110 + q * 145, 15 + q * 75, 115 + q * 15);
    }
  }
}

static inline Vec rot(const float m[3][3], const Vec &p) {
  return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z,
          m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z,
          m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z};
}

void drawBackground(float t) {
  for (int y = 0; y < HOR; y++) fb.drawFastHLine(0, y, W, skyCol[y]);

  // Funkelnde Sterne (deterministische Positionen)
  uint32_t seed = 12345;
  for (int i = 0; i < 50; i++) {
    seed = seed * 1103515245 + 12345;
    int x = (seed >> 8) % W;
    int y = (seed >> 20) % (HOR - 60);
    float b = 0.5f + 0.5f * sinf(t * 3 + i);
    fb.drawPixel(x, y, rgb(255 * b, 255 * b, 255 * b));
  }

  // Sonne mit wandernden Streifen
  const int cx = 160, cy = 100, sr = 55;
  int scroll = (int)(t * 14);
  for (int y = cy - sr; y < HOR; y++) {
    int dy = y - cy;
    if (dy > -10) {
      int gap = 1 + (dy + 10) * 6 / (HOR - cy + 10);
      if ((y + scroll) % 13 < gap) continue;
    }
    int hw = sqrtf(sr * sr - dy * dy);
    float k = (float)(y - (cy - sr)) / (2 * sr);
    fb.drawFastHLine(cx - hw, y, 2 * hw + 1, rgb(255, 230 - k * 190, 80 + k * 90));
  }

  // Boden mit Perspektivgitter
  fb.fillRect(0, HOR, W, H - HOR, rgb(12, 0, 28));
  float s = fmodf(t * 1.3f, 1.0f);
  for (int i = 0; i < 18; i++) {
    float z = i + 1 - s;
    int y = HOR + 55 / z;
    if (y >= H) continue;
    float k = min(1.0f, 1.6f / z);
    fb.drawFastHLine(0, y, W, rgb(255 * k, 20 * k, 210 * k));
  }
  for (int k = -14; k <= 14; k++)
    fb.drawLine(160 + k * 9, HOR, 160 + k * 80, H + 40, rgb(200, 10, 180));
  fb.drawFastHLine(0, HOR, W, rgb(255, 140, 220));
}

void drawTorus(float t) {
  float ax = t * 0.9f, ay = t * 0.6f, az = t * 0.3f;
  float ca = cosf(ax), sa = sinf(ax), cb = cosf(ay), sb = sinf(ay), cc = cosf(az), sc = sinf(az);
  // Rotation Z * Y * X
  const float m[3][3] = {
      {cc * cb, cc * sb * sa - sc * ca, cc * sb * ca + sc * sa},
      {sc * cb, sc * sb * sa + cc * ca, sc * sb * ca - cc * sa},
      {-sb, cb * sa, cb * ca}};

  const float dist = 4.3f, f = 150;
  const int cx = 160 + 50 * sinf(t * 0.5f), cy = 88;

  for (int i = 0; i < U * V; i++) {
    Vec p = rot(m, verts[i]);
    p.z += dist;
    view[i] = p;
    sx[i] = cx + f * p.x / p.z;
    sy[i] = cy - f * p.y / p.z;
  }

  const Vec L = {-0.45f, 0.6f, -0.66f}; // Richtung zum Licht
  int n = 0;
  for (int u = 0; u < U; u++)
    for (int v = 0; v < V; v++) {
      int i0 = u * V + v, i2 = ((u + 1) % U) * V + (v + 1) % V;
      Vec c = {(view[i0].x + view[i2].x) * 0.5f, (view[i0].y + view[i2].y) * 0.5f,
               (view[i0].z + view[i2].z) * 0.5f};
      Vec nn = rot(m, normals[i0]);
      if (nn.x * c.x + nn.y * c.y + nn.z * c.z >= 0) continue; // Rückseite

      float diff = max(0.0f, nn.x * L.x + nn.y * L.y + nn.z * L.z);
      float spec = powf(diff, 24) * 255;
      float hue = 0.5f + 0.5f * cosf(u * TWO_PI / U + t);
      float br = 0.12f + 0.88f * diff;
      faces[n++] = {c.z,
                    rgb(255 * hue * br + spec, (255 - 215 * hue) * br + spec, 255 * br + spec),
                    (uint16_t)i0};
    }

  // Maleralgorithmus: hinten zuerst
  for (int i = 1; i < n; i++) {
    Face k = faces[i];
    int j = i - 1;
    while (j >= 0 && faces[j].depth < k.depth) faces[j + 1] = faces[j], j--;
    faces[j + 1] = k;
  }

  for (int i = 0; i < n; i++) {
    int i0 = faces[i].idx, u = i0 / V, v = i0 % V;
    int i1 = ((u + 1) % U) * V + v, i2 = ((u + 1) % U) * V + (v + 1) % V, i3 = u * V + (v + 1) % V;
    fb.fillTriangle(sx[i0], sy[i0], sx[i1], sy[i1], sx[i2], sy[i2], faces[i].color);
    fb.fillTriangle(sx[i0], sy[i0], sx[i2], sy[i2], sx[i3], sy[i3], faces[i].color);
  }
}

void frame(float t) {
  drawBackground(t);
  drawTorus(t);
}
} // namespace synth

// ===========================================================================
// Szene 3: Feuerwerk - Nachleuchten durch Abdunkeln des ganzen Puffers
// ===========================================================================
namespace fireworks {
struct P {
  float x, y, vx, vy, life, maxLife;
  uint8_t r, g, b;
  bool rocket;
};
const int MAXP = 450;
P *ps = nullptr;
int n = 0;
float nextRocket = 0;
uint8_t skyline[W];
int16_t winX[160], winY[160];
int nWin = 0;

void init() {
  randomSeed(42); // reproduzierbar für Messungen
  n = 0;
  nextRocket = 0;
  int x = 0;
  while (x < W) {
    int w = random(14, 40), h = random(18, 70);
    for (int i = x; i < x + w && i < W; i++) skyline[i] = h;
    x += w + random(0, 3);
  }
  nWin = 0;
  for (int i = 0; i < 160; i++) {
    int wx = (random(W) / 4) * 4 + 1;
    if (skyline[wx] < 12) continue;
    winX[nWin] = wx;
    winY[nWin++] = H - (random(4, skyline[wx] - 4) / 5) * 5 - 4;
  }
}

bool alloc() {
  if (!ps) ps = (P *)malloc(sizeof(P) * MAXP);
  return ps != nullptr;
}

void add(const P &p) {
  if (ps && n < MAXP) ps[n++] = p;
}

void explode(const P &rk) {
  static const uint8_t cols[][3] = {{255, 60, 60}, {60, 255, 90}, {80, 140, 255},
                                    {255, 220, 60}, {255, 80, 255}, {60, 255, 255}};
  const uint8_t *c = cols[random(6)];
  const uint8_t *c2 = cols[random(6)];
  int count = random(50, 80);
  for (int i = 0; i < count; i++) {
    float a = random(1000) * TWO_PI / 1000;
    float sp = 115 * sqrtf(random(1000) / 1000.0f);
    const uint8_t *cc = (i & 3) ? c : c2;
    add({rk.x, rk.y, cosf(a) * sp + rk.vx * 0.3f, sinf(a) * sp + rk.vy * 0.3f, 0,
         random(110, 200) / 100.0f, cc[0], cc[1], cc[2], false});
  }
}

void frame(float dt) {
  // Puffer auf 75 % abdunkeln (mehrere Pixel pro 32-Bit-Wort)
  for (int y = 0; y < H; y++) {
    uint32_t *p = (uint32_t *)fb.getRow(y);
#if FB_BITS == 8
    for (int x = 0; x < W / 4; x++) { // RGB332: 4 Pixel pro Wort
      uint32_t c = p[x];
      if (c) p[x] = ((c >> 1) & 0x6D6D6D6D) + ((c >> 2) & 0x24242424);
    }
#else
    for (int x = 0; x < W / 2; x++) { // RGB565: 2 Pixel pro Wort
      uint32_t c = p[x];
      if (c) p[x] = ((c >> 1) & 0x7BEF7BEF) + ((c >> 2) & 0x39E739E7);
    }
#endif
  }

  nextRocket -= dt;
  if (nextRocket <= 0) {
    add({(float)random(40, 280), (float)H, (float)random(-30, 30), -(float)random(170, 225), 0,
         10, 255, 200, 140, true});
    nextRocket = random(25, 90) / 100.0f;
  }

  for (int i = 0; i < n;) {
    P &p = ps[i];
    p.life += dt;
    if (p.rocket) {
      p.vy += 70 * dt;
      if (p.vy > -35) {
        P rk = p;
        ps[i] = ps[--n];
        explode(rk);
        continue;
      }
    } else {
      float drag = 1 - 1.4f * dt;
      p.vx *= drag;
      p.vy = p.vy * drag + 55 * dt;
      if (p.life >= p.maxLife) {
        ps[i] = ps[--n];
        continue;
      }
    }
    p.x += p.vx * dt;
    p.y += p.vy * dt;

    if (p.rocket) {
      fb.fillRect(p.x, p.y, 2, 2, rgb(p.r, p.g, p.b));
    } else {
      float k = 1 - p.life / p.maxLife;
      if (k > 0.3f || random(3)) // Glitzern am Ende
        fb.drawPixel(p.x, p.y, rgb(p.r * k + 60 * k * k, p.g * k + 60 * k * k, p.b * k + 60 * k * k));
    }
    i++;
  }

  fb.fillCircle(272, 42, 15, rgb(235, 235, 210));
  fb.fillCircle(266, 38, 13, 0); // Sichel
  for (int x = 0; x < W; x++) fb.drawFastVLine(x, H - skyline[x], skyline[x], rgb(22, 22, 40));
  for (int i = 0; i < nWin; i++) fb.fillRect(winX[i], winY[i], 2, 2, rgb(210, 170, 70));
}
} // namespace fireworks

// ===========================================================================
// Szene 4: Multifunktionsanzeige (MFA) aus dem Auto
// ===========================================================================
namespace dash {
enum Phase { PARK, ACCEL, CRUISE, BRAKE };
const float ratio[7] = {0, 130, 76, 53, 41, 33, 28}; // U/min pro km/h
const float accel[7] = {0, 60, 45, 33, 24, 17, 11};  // km/h pro s

Phase phase;
float phaseT, speed;
int gear;
double odo = 10478.0, trip = 324.1; // bleiben über Szenenwechsel erhalten
const int DW = ILI9341_TFTWIDTH, DH = ILI9341_TFTHEIGHT; // Hochformat

void init() {
  phase = PARK;
  phaseT = speed = 0;
  gear = 1;
}

void textRight(const GFXfont *f, uint8_t size, const char *s, int xr, int baseline, uint16_t c) {
  int16_t bx, by;
  uint16_t bw, bh;
  fb.setFont(f);
  fb.setTextSize(size);
  fb.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  fb.setTextColor(c);
  fb.setCursor(xr - bw - bx, baseline);
  fb.print(s);
  fb.setTextSize(1);
}

void textLeft(const GFXfont *f, const char *s, int x, int baseline, uint16_t c) {
  fb.setFont(f);
  fb.setTextColor(c);
  fb.setCursor(x, baseline);
  fb.print(s);
}

void simulate(float dt) {
  phaseT += dt;
  switch (phase) {
  case PARK:
    speed = 0;
    gear = 1;
    if (phaseT >= 2.5f) phase = ACCEL, phaseT = 0;
    break;
  case ACCEL:
    speed += accel[gear] * dt;
    if (speed * ratio[gear] > 6300 && gear < 6) gear++;
    if (speed >= 210) phase = CRUISE, phaseT = 0;
    break;
  case CRUISE:
    speed = 210 + 1.5f * sinf(phaseT * 2);
    if (phaseT >= 2.0f) phase = BRAKE, phaseT = 0;
    break;
  case BRAKE:
    speed = max(0.0f, speed - 55 * dt);
    if (speed <= 0) phase = PARK, phaseT = 0;
    break;
  }
  double km = speed * dt / 3600.0;
  odo += km;
  trip += km;
}

void frame(float dt) {
  simulate(dt);

  const uint16_t white = rgb(245, 245, 245);
  const int X0 = 10, X1 = DW - 10;
  fb.fillScreen(ILI9341_BLACK);

  char buf[16];
  uint32_t minutes = 15 * 60 + 25 + millis() / 60000;
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (minutes / 60) % 24, minutes % 60);
  textLeft(&FreeSansBold18pt7b, buf, X0 + 14, 40, white);
  fb.fillRect(X0, 52, X1 - X0, 2, white);

  // Große Geschwindigkeit, doppelt skaliert für den pixeligen LCD-Look
  snprintf(buf, sizeof(buf), "%d", (int)speed);
  textRight(&FreeSansBold18pt7b, 2, buf, 180, 152, white);
  textLeft(&FreeSansBold9pt7b, "km/h", 185, 152, white);
  fb.fillRect(X0, 200, X1 - X0, 2, white);

  // Außentemperatur mit selbst gezeichnetem Gradzeichen
  textRight(&FreeSansBold12pt7b, 1, "19.0", 128, 232, white);
  fb.drawCircle(137, 217, 3, white);
  fb.drawCircle(137, 217, 2, white);
  textLeft(&FreeSansBold9pt7b, "C", 143, 232, white);
  fb.fillRect(X0, 246, X1 - X0, 2, white);

  textLeft(&FreeSans9pt7b, "km", X0 + 2, 268, white);
  textRight(&FreeSans9pt7b, 1, "trip", X1 - 2, 268, white);
  snprintf(buf, sizeof(buf), "%lu", (uint32_t)odo);
  textLeft(&FreeSansBold18pt7b, buf, X0 + 2, 304, white);
  snprintf(buf, sizeof(buf), "%.1f", trip);
  textRight(&FreeSansBold18pt7b, 1, buf, X1 - 2, 304, white);

  fb.setFont();
}
} // namespace dash

// ===========================================================================
// Szene 5: Diagramm mit Hardware-Scrolling
// Zwei Messkurven laufen nach links. Das Display verschiebt den Inhalt selbst,
// übertragen wird nur die neue Spalte rechts. Links eine feste Achse.
// ===========================================================================
namespace chart {
const int FIXED = 48; // feste Achsenspalte links
const int STEP = 2;   // Pixel pro Frame
float t;
int tick;
int16_t prev1, prev2;

uint16_t bg() { return rgb(6, 12, 16); }
uint16_t grid() { return rgb(28, 60, 44); }

void drawAxis() {
  fb.fillRect(0, 0, FIXED, H, rgb(18, 22, 30));
  fb.drawFastVLine(FIXED - 1, 0, H, rgb(90, 110, 130));
  fb.setFont();
  fb.setTextSize(1);
  fb.setTextColor(rgb(170, 190, 210));
  const char *labels[] = {"+100", "+50", "0", "-50", "-100"};
  for (int i = 0; i < 5; i++) {
    int gy = 20 + i * 50;
    fb.drawFastHLine(FIXED - 6, gy, 5, rgb(170, 190, 210));
    fb.setCursor(6, gy - 3);
    fb.print(labels[i]);
  }
}

void init() {
  fb.setScrollArea(FIXED, 0);
  fb.fillRect(FIXED, 0, W - FIXED, H, bg());
  for (int gy = 20; gy < H; gy += 50) fb.drawFastHLine(FIXED, gy, W - FIXED, grid());
  t = 0;
  tick = 0;
  prev1 = prev2 = 120;
}

void segment(int x, int16_t a, int16_t b, uint16_t c) {
  int16_t lo = min(a, b), hi = max(a, b);
  fb.drawFastVLine(x, lo - 1, hi - lo + 3, c);
}

void frame() {
  fb.scroll(STEP);
  for (int c = 0; c < STEP; c++) {
    int x = W - STEP + c;
    tick++;
    t += 0.035f;
    fb.drawFastVLine(x, 0, H, bg());
    for (int gy = 20; gy < H; gy += 50) fb.drawPixel(x, gy, grid());
    if (tick % 50 == 0)
      for (int gy = 0; gy < H; gy += 3) fb.drawPixel(x, gy, grid());
    // EKG-artige Kurve und eine langsame Schwingung
    float ph = fmodf(t, 3.0f);
    float ecg = ph < 0.08f ? -95 * sinf(ph / 0.08f * PI) : ph < 0.16f ? 30 * sinf((ph - 0.08f) / 0.08f * PI) : 0;
    int16_t y1 = 120 + ecg + 6 * sinf(t * 1.7f);
    int16_t y2 = 120 + 80 * sinf(t * 0.6f) * cosf(t * 0.17f);
    segment(x, prev2, y2, rgb(255, 205, 60));
    segment(x, prev1, y1, rgb(70, 255, 130));
    prev1 = y1;
    prev2 = y2;
  }
  drawAxis();
}
} // namespace chart

// ===========================================================================
// Szenensteuerung
// ===========================================================================
enum Scene { PLASMA, SYNTH, FIREWORKS, DASH, CHART, NUM_SCENES };
const char *sceneNames[] = {"Plasma", "Synthwave", "Feuerwerk", "Bordcomputer", "Diagramm"};
const uint32_t sceneMs[] = {12000, 12000, 12000, 20000, 15000};

uint8_t scene = PLASMA;
uint32_t sceneStart = 0;
uint32_t fps = 0;
bool autoCycle = true;
bool shotListener = false; // tftctl.py --listen hat sich angemeldet
uint32_t spiFreq = 40000000;

void drawTitle(const char *text) {
  int16_t bx, by;
  uint16_t bw, bh;
  fb.setFont(&FreeSansBold12pt7b);
  fb.setTextSize(1);
  fb.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  int16_t x = (fb.width() - bw) / 2 - bx, y = (fb.height() - bh) / 2 - by;
  fb.setTextColor(ILI9341_BLACK);
  fb.setCursor(x + 2, y + 2);
  fb.print(text);
  fb.setTextColor(ILI9341_WHITE);
  fb.setCursor(x, y);
  fb.print(text);
  fb.setFont();
}

void drawHud() {
  fb.setFont();
  fb.setTextSize(1);
  fb.fillRect(0, 0, 46, 11, ILI9341_BLACK);
  fb.setTextColor(ILI9341_WHITE);
  fb.setCursor(2, 2);
  fb.printf("%2lu fps", fps);
}

void startScene(uint8_t s) {
  scene = s;
  sceneStart = millis();
  fb.setRotation(scene == DASH ? 0 : 1); // MFA im Hochformat
#if FB_BITS == 8
  if (scene == PLASMA) fb.setPalette(plasma::pal); // Plasma schreibt Palettenindizes
  else fb.useRGB332();
#endif
  fb.fillScreen(ILI9341_BLACK);
  if (scene == FIREWORKS) fireworks::init();
  if (scene == DASH) dash::init();
  if (scene == CHART) chart::init();
}

void setup() {
  Serial.begin(921600);
  pinMode(0, INPUT_PULLUP); // BOOT-Taste: Screenshot
  analogWrite(TFT_BL, 255); // Helligkeit 0..255
  // Reihenfolge: erst die Partikel, dann so viele DMA-Puffer wie passen
  if (!fireworks::alloc()) Serial.println("Nicht genug RAM fuer die Feuerwerk-Partikel!");
  tft.begin(spiFreq, 10); // bis zu 10 DMA-Puffer à 6,4 KB
  spiFreq = tft.autoSpeed(); // höchsten fehlerfreien Takt per Readback ermitteln
  Serial.printf("INFO auto_speed_mhz=%.2f\n", spiFreq / 1e6f);
  if (!spiFreq) spiFreq = tft.spiSpeed();

  if (!fb.begin()) {
    Serial.println("Nicht genug RAM fuer den Framebuffer!");
    while (true) delay(1000);
  }
  fb.setRotation(1);
  fb.setVSync(true); // tearing-frei, synchron zur Bildwiederholung (~58 Hz)
  Serial.printf("INFO dma_buffers=%u heap=%lu maxblk=%u\n", tft.dmaBufferCount(), ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  plasma::init();
  synth::init();
  startScene(PLASMA);
}

// ===========================================================================
// Serielle Schnittstelle (921600 Baud, Befehle mit Zeilenende):
//   s<n>   Szene n festhalten (0=Plasma, 1=Synthwave, 2=Feuerwerk, 3=Bordcomputer, 4=Diagramm)
//   a      automatischer Szenenwechsel
//   f<MHz> SPI-Takt setzen, z.B. f80
//   b      Benchmark der Einzelschritte
// Jede Sekunde kommt eine Zeile:
//   STAT scene=Plasma fps=19 render_us=... display_us=... px=... spi_mhz=40 heap=...
// ===========================================================================
void benchmark() {
  const int N = 10;
  uint32_t t0 = micros();
  for (int i = 0; i < N; i++) plasma::frame(millis());
  uint32_t tRender = (micros() - t0) / N;

  t0 = micros();
  for (int i = 0; i < N; i++) {
    tft.startWrite();
    tft.setAddrWindow(0, 0, W, H);
#if FB_BITS == 16
    for (int y = 0; y < H; y++) tft.writePixels(fb.getRow(y), W);
#else
    tft.writeColor(0x1234, (uint32_t)W * H); // 8 Bit: reine Busmessung
#endif
    tft.endWrite();
    tft.finishWrites();
  }
  uint32_t tRaw = (micros() - t0) / N;

  t0 = micros();
  for (int i = 0; i < N; i++) {
    fb.invalidate();
    fb.display();
    tft.finishWrites();
  }
  uint32_t tFull = (micros() - t0) / N;

  t0 = micros();
  for (int i = 0; i < N; i++) {
    fb.display(); // nichts geändert -> nur Hashes
    tft.finishWrites();
  }
  uint32_t tHash = (micros() - t0) / N;

  // Kosten eines Adressfensters: 1x1-Pixel schreiben (CASET+PASET+RAMWR+Daten)
  t0 = micros();
  tft.startWrite();
  for (int i = 0; i < 1000; i++) tft.writePixel(i % W, (i / W) * 7, 0x1234);
  tft.endWrite();
  tft.finishWrites();
  uint32_t tWin = micros() - t0; // 1000 Fenster -> Wert in ns pro Fenster

  float ideal = W * H * 16.0f / spiFreq * 1e6f;
  Serial.printf("BENCH spi_mhz=%lu plasma_render_us=%lu raw_transfer_us=%lu (ideal %.0f, %.0f%%) "
                "full_display_us=%lu hash_only_us=%lu window_ns=%lu (=%lu px)\n",
                spiFreq / 1000000, tRender, tRaw, ideal, ideal * 100 / tRaw, tFull, tHash, tWin,
                (uint32_t)(tWin / (16000.0f / (spiFreq / 1000000))));
}

void renderScene(uint32_t now, float dt) {
  uint32_t sceneMs = now - sceneStart;
  switch (scene) {
  case PLASMA:    plasma::frame(now); break;
  case SYNTH:     synth::frame(sceneMs / 1000.0f); break;
  case FIREWORKS: fireworks::frame(dt); break;
  case DASH:      dash::frame(dt); break;
  case CHART:     chart::frame(); break;
  }
  if (sceneMs < 2000 && scene != CHART) drawTitle(sceneNames[scene]); // Titel würde mitscrollen
  drawHud();
}

// Liest das komplette Display-RAM zurück und vergleicht es mit dem Framebuffer
uint32_t verifyScreen(bool quiet = false);
uint32_t verifyScreen(bool quiet) {
  tft.finishWrites();
  static uint16_t line[ILI9341_TFTHEIGHT];
  const int16_t w = fb.width(), h = fb.height();
  uint32_t badPx = 0, badRows = 0;
  int16_t firstBad = -1;
  for (int y = 0; y < h; y++) {
    fb.readDisplayRow(y, line); // was tatsächlich angezeigt wird (inkl. Scrollen)
    uint32_t bad = 0;
    for (int x = 0; x < w; x++) {
      uint16_t want = fb.getPixel(x, y);
      if (line[x] != want && badPx + bad < 8 && !quiet)
        Serial.printf("  (%d,%d) soll=%04X ist=%04X xor=%04X\n", x, y, want, line[x], want ^ line[x]);
      bad += line[x] != want;
    }
    if (bad) {
      badRows++;
      if (firstBad < 0) firstBad = y;
    }
    badPx += bad;
  }
  if (!quiet || badPx)
    Serial.printf("VERIFY spi_mhz=%lu bad_px=%lu bad_rows=%lu first_bad_row=%d\n", spiFreq / 1000000,
                  badPx, badRows, firstBad);
  return badPx;
}

template <typename F>
static void hashBench(const char *name, uint32_t *store, int segs, F fn) {
  const int rows = 1920 / segs; // Teilmenge, auf Vollbild hochgerechnet
  uint32_t t0 = micros(), acc = 0;
  for (int y = 0; y < rows; y++) {
    const uint32_t *row = (const uint32_t *)fb.getRow(y);
    uint32_t *hs = store + y * segs;
    for (int s = 0; s < segs; s++) {
      uint32_t h = fn(row + s * 8);
      acc += h != hs[s];
      hs[s] = h;
    }
  }
  Serial.printf("HASH %-22s %5lu us/Bild (%lu)\n", name, (micros() - t0) * fb.height() / rows, acc);
}

// Testmuster für den Direktmodus-Vergleich (gleiche Befehle auf tft und fb)
template <class G> void drawPattern(G &g) {
  g.fillScreen(0x0841);
  uint32_t seed = 7;
  auto rnd = [&](int m) { seed = seed * 1103515245 + 12345; return (int)((seed >> 8) % m); };
  for (int i = 0; i < 60; i++) g.drawLine(rnd(400) - 40, rnd(300) - 30, rnd(400) - 40, rnd(300) - 30, rnd(65536));
  for (int i = 0; i < 30; i++) g.drawCircle(rnd(360) - 20, rnd(280) - 20, rnd(60), rnd(65536));
  for (int i = 0; i < 20; i++) g.drawRoundRect(rnd(340) - 10, rnd(260) - 10, rnd(120), rnd(90), rnd(20), rnd(65536));
  for (int i = 0; i < 15; i++) g.fillCircle(rnd(320), rnd(240), rnd(25), rnd(65536));
  for (int i = 0; i < 10; i++) g.fillTriangle(rnd(320), rnd(240), rnd(320), rnd(240), rnd(320), rnd(240), rnd(65536));
  g.setFont();
  g.setTextSize(2);
  g.setTextColor(0xFFFF, 0x001F);
  g.setCursor(5, 5);
  g.print("Direkt 123");
  g.setTextSize(1);
  g.setTextColor(0xFFE0);
  g.setCursor(-3, 200);
  g.print("transparent am Rand, mit Umbruch und noch mehr Text dahinter");
  g.setFont(&FreeSansBold9pt7b);
  g.setTextColor(0x07FF);
  g.setCursor(30, 120);
  g.print("FreeSans 9pt");
  g.setFont();
}

void handleSerial() {
  static char buf[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(buf) - 1) buf[len++] = c;
      continue;
    }
    if (!len) continue;
    buf[len] = 0;
    len = 0;
    switch (buf[0]) {
    case 's':
      autoCycle = false;
      startScene(constrain(atoi(buf + 1), 0, NUM_SCENES - 1));
      break;
    case 'a':
      autoCycle = true;
      break;
    case 'f':
      spiFreq = atoi(buf + 1) * 1000000UL;
      tft.setSPISpeed(spiFreq);
      fb.invalidate();
      break;
    case 'b':
      benchmark();
      break;
#if FB_BITS == 16
    case 'h': { // Mikro-Benchmark: Hash-Varianten über den ganzen Framebuffer
      const int segs = fb.width() / 16;
      uint32_t *dram = (uint32_t *)heap_caps_malloc(1920 * 4, MALLOC_CAP_8BIT);
      uint32_t *iram = (uint32_t *)heap_caps_malloc(1920 * 4, MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
      if (!dram) {
        Serial.println("ERR kein RAM");
        heap_caps_free(iram);
        break;
      }
      auto fnv1 = [](const uint32_t *q) {
        uint32_t h = 0x811C9DC5;
        for (int i = 0; i < 8; i++) h = (h ^ q[i]) * 0x01000193;
        return h;
      };
      auto fnv2 = [](const uint32_t *q) {
        uint32_t a = 0x811C9DC5, b = 0x050C5D1F;
        for (int i = 0; i < 8; i += 2) a = (a ^ q[i]) * 0x01000193, b = (b ^ q[i + 1]) * 0x01000193;
        return a ^ (b * 0x9E3779B1);
      };
      auto mix = [](const uint32_t *q) { // Rotieren+Addieren, positionsabhängig
        uint32_t h = 0x811C9DC5;
        for (int i = 0; i < 8; i++) h = ((h << 5) | (h >> 27)) + (q[i] ^ (0x9E3779B9 * (i + 1)));
        return h;
      };
      auto copyOnly = [](const uint32_t *q) {
        return q[0] ^ q[1] ^ q[2] ^ q[3] ^ q[4] ^ q[5] ^ q[6] ^ q[7];
      };
      hashBench("fnv1 dram", dram, segs, fnv1);
      if (iram) hashBench("fnv1 iram", iram, segs, fnv1);
      hashBench("fnv2 dram", dram, segs, fnv2);
      hashBench("rotadd dram", dram, segs, mix);
      hashBench("xor-only dram (Lesen)", dram, segs, copyOnly);
      heap_caps_free(iram);
      heap_caps_free(dram);
      break;
    }
    case 'y': { // VSync an/aus: y1 / y0
      bool ok = fb.setVSync(atoi(buf + 1));
      Serial.printf("VSYNC %s refresh_hz=%.1f\n", fb.vsync() ? "an" : (ok ? "aus" : "FEHLER"),
                    fb.refreshHz());
      break;
    }
    case 'o': // Rotation der aktuellen Szene ändern (Test): o0..o3
      fb.setRotation(atoi(buf + 1));
      break;
#endif
    case 'S': { // Profil der Synthwave-Szene
      const int N = 20;
      uint32_t t0 = micros();
      for (int i = 0; i < N; i++) synth::drawBackground(1.0f + i * 0.01f);
      uint32_t tBg = (micros() - t0) / N;
      t0 = micros();
      for (int i = 0; i < N; i++) synth::drawTorus(1.0f + i * 0.01f);
      uint32_t tTorus = (micros() - t0) / N;
      Serial.printf("PROFILE synth background_us=%lu torus_us=%lu\n", tBg, tTorus);
      break;
    }
    case 'w': // Fensterkosten für display() in Pixeln
      fb.setWindowCost(atoi(buf + 1));
      break;
    case 'r':
      Serial.printf("REG madctl=0x%02X pixfmt=0x%02X selfdiag=0x%02X\n",
                    tft.readcommand8(ILI9341_RDMADCTL), tft.readcommand8(ILI9341_RDPIXFMT),
                    tft.readcommand8(ILI9341_RDSELFDIAG));
      break;
    case 'v':
      verifyScreen();
      break;
    case 'D': { // Direktmodus prüfen: Muster direkt zeichnen, mit Framebuffer-Version vergleichen
      uint32_t t0 = micros();
      drawPattern(tft);
      tft.finishWrites();
      uint32_t tDirect = micros() - t0;
      drawPattern(fb); // nur RAM, kein display()
      uint32_t bad = verifyScreen(false);
      Serial.printf("DIRECT bad_px=%lu zeit_us=%lu\n", bad, tDirect);
      fb.invalidate();
      break;
    }
    case 'T': { // T<n>: gepackten Modus n-mal per Readback prüfen; TA / TO: gepackten Modus an / aus
      if (buf[1] == 'A' || buf[1] == 'O') {
        tft.setPacked(buf[1] == 'A');
        Serial.printf("PACKED %d\n", tft.packed());
        break;
      }
      int n = max(1, atoi(buf + 1));
      uint32_t bad = 0;
      for (int i = 0; i < n; i++) bad += tft.verifyPacked(esp_random());
      Serial.printf("PACKED runs=%d bad_px=%lu\n", n, bad);
      fb.invalidate();
      break;
    }
    case 'L': // L1: Screenshot-Empfänger ist verbunden (BOOT-Taste schickt dann Bilder)
      shotListener = atoi(buf + 1);
      break;
    case 'P': // Screenshot (siehe fb.screenshot)
      // Direkt nach einem Befehl vom Mac gehen die ersten Antwort-Bytes über
      // die USB-Seriell-Strecke gelegentlich verloren (gemessen: Kopfzeile
      // fehlte in 4 von 6 Fällen, mit 50 ms Pause in 0 von 8)
      delay(50);
      fb.screenshot(Serial);
      break;
    case 'i': // Idle-Modus (8 Farben): i1 / i0
      tft.setIdleMode(atoi(buf + 1));
      break;
    case 'p': { // Teilanzeige: p0 = aus, p<erste>,<letzte> = Panel-Zeilen
      int a = atoi(buf + 1);
      char *comma = strchr(buf, ',');
      if (!comma) tft.setPartialMode(false);
      else tft.setPartialMode(true, a, atoi(comma + 1));
      break;
    }
    case 'z': // Schlafmodus: z1 / z0
      tft.enableSleep(atoi(buf + 1));
      break;
    case 'V': { // V<n>: n Frames rendern und jeden einzelnen komplett zurücklesen
      int n = constrain(atoi(buf + 1), 1, 1000), badFrames = 0;
      uint32_t badTotal = 0, t = millis();
      for (int i = 0; i < n; i++) {
        t += 9; // simulierte Frame-Zeit ~110 fps
        renderScene(t, 0.009f);
        fb.display();
        uint32_t b = verifyScreen(true);
        badTotal += b;
        badFrames += b != 0;
      }
      Serial.printf("VERIFYN frames=%d bad_frames=%d bad_px=%lu\n", n, badFrames, badTotal);
      break;
    }
    case 't': { // Treibertest: Register-Transfers vs. DMA, jeweils zurücklesen
      uint16_t px[8];
      tft.drawPixel(5, 5, 0xF800); // nur kleine Transfers (Polling)
      tft.readRect(5, 5, 1, 1, px);
      Serial.printf("TEST poll_pixel soll=F800 ist=%04X\n", px[0]);
      tft.fillRect(10, 10, 100, 40, 0x07E0); // DMA
      tft.drawPixel(6, 5, 0x001F);           // kleiner Transfer direkt nach DMA
      tft.finishWrites();
      tft.readRect(10, 10, 4, 1, px);
      tft.readRect(6, 5, 1, 1, px + 4);
      Serial.printf("TEST dma_rect soll=07E0 ist=%04X %04X %04X %04X  pixel_nach_dma soll=001F ist=%04X\n",
                    px[0], px[1], px[2], px[3], px[4]);
      tft.startWrite(); // viele gemischte Transfers über die Interrupt-Liste
      for (int i = 0; i < 8; i++) {
        tft.writeFillRect(20 + i * 10, 60, 8, 8, 0x1111 * (i + 1));
        tft.writePixel(20 + i * 10, 70, 0xFFFF - i);
      }
      tft.endWrite();
      tft.finishWrites();
      for (int i = 0; i < 8; i++) {
        uint16_t a, b;
        tft.readRect(20 + i * 10, 60, 1, 1, &a);
        tft.readRect(20 + i * 10, 70, 1, 1, &b);
        Serial.printf("TEST isr[%d] rect soll=%04X ist=%04X  pixel soll=%04X ist=%04X\n", i,
                      0x1111 * (i + 1), a, 0xFFFF - i, b);
      }
      fb.invalidate();
      break;
    }
    case 'c': { // Rohbefehl: c<cmd> <param> ... (hex), z.B. cB1 00 18
      uint8_t bytes[16];
      int nb = 0;
      for (char *p = strtok(buf + 1, " "); p && nb < 16; p = strtok(nullptr, " "))
        bytes[nb++] = strtol(p, nullptr, 16);
      if (nb) tft.sendCommand(bytes[0], bytes + 1, nb - 1);
      fb.invalidate();
      break;
    }
    case 'R': { // Register lesen: R<cmd> <anzahl> (hex/dez), z.B. R45 3
      char *end;
      uint8_t cmd = strtol(buf + 1, &end, 16), n = constrain(atoi(end), 1, 8), out[8];
      tft.readRegister(cmd, out, n);
      Serial.printf("REG 0x%02X:", cmd);
      for (int i = 0; i < n; i++) Serial.printf(" %02X", out[i]);
      Serial.println();
      break;
    }
    case 'g': { // Scanline abtasten: Zeilenanzahl, Zeilen- und Bildfrequenz
      tft.finishWrites();
      const int N = 400;
      static uint16_t line[N];
      static uint32_t ts[N];
      uint16_t maxLine = 0;
      for (int i = 0; i < N; i++) {
        line[i] = tft.getScanline();
        ts[i] = micros();
        if (line[i] > maxLine) maxLine = line[i];
        delayMicroseconds(1500);
      }
      // Fortschritt in Zeilen über alle Samples (mit Umlauf bei maxLine+1)
      uint32_t total = maxLine + 1, adv = 0;
      for (int i = 1; i < N; i++)
        adv += line[i] >= line[i - 1] ? line[i] - line[i - 1] : line[i] + total - line[i - 1];
      float lineUs = (float)(ts[N - 1] - ts[0]) / adv;
      Serial.printf("SCAN lines=%lu line_us=%.2f frame_us=%.0f hz=%.2f\n", total, lineUs,
                    lineUs * total, 1e6f / (lineUs * total));
      break;
    }
    default:
      Serial.printf("ERR unbekannter Befehl: %s\n", buf);
      return;
    }
    Serial.printf("OK %s\n", buf);
  }
}

void loop() {
  static uint32_t last = millis(), frames = 0, fpsTimer = millis();
  static uint64_t renderSum = 0, displaySum = 0;

  handleSerial();

  // BOOT-Taste (GPIO 0) gedrückt -> Screenshot an den Mac. Binärdaten nur,
  // wenn tftctl.py --listen zuhört; sonst sähe man im Monitor nur Zeichensalat.
  static bool lastButton = HIGH;
  bool button = digitalRead(0);
  if (lastButton == HIGH && button == LOW) {
    if (shotListener) fb.screenshot(Serial);
    else Serial.println("Screenshot: Monitor schliessen und 'tftctl.py --listen <ordner>' starten");
  }
  lastButton = button;

  uint32_t now = millis();
  float dt = min((now - last) / 1000.0f, 0.1f);
  last = now;

  if (autoCycle && now - sceneStart >= sceneMs[scene]) startScene((scene + 1) % NUM_SCENES);
  uint32_t sceneMs = now - sceneStart;

  uint32_t t0 = micros();
  renderScene(now, dt);
  uint32_t t1 = micros();
  fb.display();
  uint32_t t2 = micros();
  renderSum += t1 - t0;
  displaySum += t2 - t1;

  frames++;
  if (now - fpsTimer >= 1000) {
    Serial.printf("STAT scene=%s fps=%lu render_us=%lu display_us=%lu px=%lu spi_mhz=%lu heap=%lu "
                  "maxblk=%u vsync=%d late=%lu\n",
                  sceneNames[scene], frames, (uint32_t)(renderSum / frames),
                  (uint32_t)(displaySum / frames), fb.lastPixelsSent(), spiFreq / 1000000,
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                  fb.vsync(), fb.vsyncLate());
    fps = frames;
    frames = 0;
    renderSum = displaySum = 0;
    fpsTimer = now;
  }
}
