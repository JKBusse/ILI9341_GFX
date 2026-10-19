// Video-Streaming-Demo: der ESP32 holt einen MJPEG-Stream über WLAN (HTTP
// oder HTTPS) und zeigt ihn auf dem ILI9341 an.
//
// Der ESP streamt selbst, ohne PC dazwischen. Quelle ist jede URL, die MJPEG
// liefert (Baseline-JPEGs mit eigenen Huffman-Tabellen):
//   - Live-Streams (multipart/x-mixed-replace), z.B. IP-Kameras: die Quelle
//     gibt das Tempo vor
//   - Dateien: rohes .mjpeg (JPEGs hintereinander) oder .avi mit MJPEG; das
//     Tempo kommt aus dem AVI-Kopf bzw. STREAM_FPS, am Ende geht es von vorn los
// H.264 (YouTube, MP4) schafft der ESP32 nicht. Größere Bilder werden um 1/2,
// 1/4 oder 1/8 verkleinert, kleinere zentriert.
//
// Tearing: Im Querformat baut das Panel das Bild spaltenweise auf (entlang
// der 320er-Achse), JPEGDEC liefert es aber zeilenweise - die Richtungen
// kreuzen sich, jeder Frame reißt irgendwo. Deshalb gilt: Ist ein Frame höher
// als breit, wird er als um 90° (im Uhrzeigersinn) gedrehtes Querformat-Bild
// behandelt und in Hochformat-Koordinaten gezeichnet. Dann läuft das Schreiben
// in Richtung des Bildaufbaus: Jeder Frame startet, sobald der Bildaufbau oben
// beginnt, und bleibt hinter ihm (Panel auf ~50 Hz, ein Frame braucht ~26 ms
// < 2 Refreshes). So erscheint jeder Frame vollständig in einem Refresh.
// Ein Video passend umwandeln, z.B. 25 fps (= jeder zweite Refresh):
//   ffmpeg -i in.mp4 -an -vf "fps=25,scale=320:180,crop=320:176,transpose=clock" \
//          -pix_fmt yuvj420p -q:v 7 -f mjpeg video.mjpeg
// und auf einen beliebigen Webserver legen.
//
// Aufbau:
//   Core 0: Netzwerk-Task - liest den Stream, schneidet die JPEGs an den
//           Markern SOI (FFD8) / EOI (FFD9) aus, legt sie in einen freien Puffer
//   Core 1: loop() - dekodiert mit JPEGDEC direkt in die DMA-Queue des Displays
// Drei Frame-Puffer: während einer dekodiert wird, laden die anderen. Ist keiner
// frei, wartet der Netzwerk-Task (TCP bremst dann die Quelle).
//
// Bauen: WIFI_SSID=... WIFI_PASS=... STREAM_URL=http://... \
//        pio run -e videostream -t upload
// Kein Framebuffer: der Heap wird für WLAN/TLS gebraucht.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include "ILI9341_GFX.h"

#define TFT_CS  15
#define TFT_DC  2
#define TFT_RST 4
#define TFT_BL  13

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef STREAM_URL
#define STREAM_URL "" // eigene Quelle; gesetzt kommt sie als erste in die Liste
#endif

// Quellen, mit der BOOT-Taste durchschaltbar.
// Big Buck Bunny: 2 Minuten als MJPEG (320x176, gedreht gespeichert als
// 176x320, 25 fps), Asset des Releases "demo-media" dieses Repos.
// Big Buck Bunny (c) 2008 Blender Foundation | www.bigbuckbunny.org, CC BY 3.0.
// Die Kameras sind öffentliche Live-Kameras (geprüft 10/2026: Baseline-JPEG mit
// Huffman-Tabellen, Frames < 16 KB; können jederzeit offline gehen). Die
// Hannover-Kameras (zander-info.de) schicken ohne compression=50 bis 58 KB
// große Frames, die verworfen würden.
struct Source {
  const char *name, *url;
};
static const Source SOURCES[] = {
    {"Big Buck Bunny",
     "https://github.com/JKBusse/ILI9341_GFX/releases/download/demo-media/bigbuckbunny_176x320_25fps_rot.mjpeg"},
    {"Hannover Hbf", "http://mzinfo.dnshome.de:5010/axis-cgi/mjpg/video.cgi?resolution=320x240&compression=50"},
    {"Hannover Hamburger Allee",
     "http://mzinfo.dnshome.de:5011/axis-cgi/mjpg/video.cgi?resolution=320x240&compression=50"},
    {"Hannover Raschplatz", "http://mzinfo.dnshome.de:5009/axis-cgi/mjpg/video.cgi?resolution=320x240&compression=50"},
    {"Hannover Wetterkamera",
     "http://mzinfo.dnshome.de:5012/axis-cgi/mjpg/video.cgi?resolution=320x240&compression=50"},
    {"Pajala (S), Marktplatz", "https://195.196.36.242/mjpg/video.mjpg?resolution=320x240"},
    {"Italien, Kathedrale", "http://88.53.197.250/axis-cgi/mjpg/video.cgi?resolution=320x240"},
    {"Tokio", "http://61.211.241.239/nphMotionJpeg?Resolution=320x240&Quality=Standard"},
};
static const uint8_t NUM_SOURCES = sizeof(SOURCES) / sizeof(SOURCES[0]);
static const bool OWN_URL = *STREAM_URL;
static const uint8_t NUM_CHOICES = NUM_SOURCES + OWN_URL;
static const Source &source(uint8_t i) {
  static const Source own = {"Eigene Quelle", STREAM_URL};
  return OWN_URL ? (i == 0 ? own : SOURCES[i - 1]) : SOURCES[i];
}
static volatile uint8_t curSource = 0; // loop() setzt, Netzwerk-Task folgt
#ifndef STREAM_FPS
#define STREAM_FPS 25 // Wiedergaberate für .mjpeg-Dateien (AVI: aus dem Dateikopf)
#endif

static const uint32_t FRAME_BUF = 16 * 1024; // größtes JPEG (größere werden verworfen)
static const uint8_t NUM_BUFS = 3;            // zusammen 48 KB, mehr lässt TLS zu wenig Heap

ILI9341_GFX tft(TFT_CS, TFT_DC, TFT_RST);
JPEGDEC jpeg;

struct Frame {
  uint8_t *data;
  uint32_t len;
  uint8_t src; // Quelle, aus der der Frame stammt (alte nach dem Umschalten verwerfen)
};
static QueueHandle_t freeQ, fullQ; // leere Puffer -> Netzwerk, volle Frames -> Decoder

// Statistik (Netzwerk-Task schreibt, loop() liest)
static volatile uint32_t statBytes = 0, statDropped = 0, statConnects = 0;
static volatile bool streaming = false;
static volatile uint32_t frameUs = 0; // Abstand der Frames bei Dateien, 0 = Live-Stream (nicht bremsen)
static char statusMsg[96] = "";
static volatile bool statusDirty = false;

static void setStatus(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(statusMsg, sizeof(statusMsg), fmt, ap);
  va_end(ap);
  Serial.printf("INFO %s\n", statusMsg);
  statusDirty = true;
}

// ---------------------------------------------------------------------------
// Netzwerk (Core 0)
// ---------------------------------------------------------------------------

/** Wartet ms, bricht aber ab, sobald eine andere Quelle gewählt wurde. */
static bool waitUnlessSwitched(uint8_t src, uint32_t ms) {
  for (uint32_t t = millis(); millis() - t < ms; vTaskDelay(pdMS_TO_TICKS(20)))
    if (curSource != src) return false;
  return true;
}

/**
 * Liest einen verbundenen Stream und schneidet die JPEGs heraus. Kehrt zurück,
 * wenn die Verbindung endet oder eine andere Quelle gewählt wurde.
 */
static void pumpStream(WiFiClient *s, HTTPClient &http, uint8_t src) {
  static uint8_t rx[4096];
  Frame cur = {nullptr, 0, src};
  bool prevFF = false; // letztes Byte außerhalb eines Frames war 0xFF
  uint32_t lastData = millis();
  bool first = true;

  while ((http.connected() || s->available()) && curSource == src) {
    int avail = s->available();
    if (avail <= 0) {
      if (millis() - lastData > 5000) break; // Quelle hängt
      vTaskDelay(1);
      continue;
    }
    int n = s->read(rx, min(avail, (int)sizeof(rx)));
    if (n <= 0) continue;
    lastData = millis();
    if (first && n >= 36 && !memcmp(rx, "RIFF", 4) && !memcmp(rx + 8, "AVI ", 4) &&
        !memcmp(rx + 24, "avih", 4)) {
      uint32_t us;
      memcpy(&us, rx + 32, 4); // dwMicroSecPerFrame
      if (us >= 1000 && us <= 1000000) frameUs = us;
      Serial.printf("INFO AVI, %.1f fps\n", 1e6f / frameUs);
    }
    first = false;
    statBytes += n;

    int i = 0;
    while (i < n) {
      if (!cur.data) {
        // Frame-Anfang suchen: FF D8
        for (; i < n; i++) {
          if (prevFF && rx[i] == 0xD8) break;
          prevFF = rx[i] == 0xFF;
        }
        if (i >= n) break;
        i++;
        prevFF = false;
        // Warten, bis der Decoder einen Puffer frei gibt
        while (xQueueReceive(freeQ, &cur, pdMS_TO_TICKS(50)) != pdTRUE)
          if (curSource != src) return;
        cur.src = src;
        cur.data[0] = 0xFF;
        cur.data[1] = 0xD8;
        cur.len = 2;
      }
      // In den Frame kopieren und darin nach FF D9 suchen (auch über Blockgrenzen)
      uint32_t take = n - i;
      if (cur.len + take > FRAME_BUF) { // zu groß: verwerfen
        statDropped++;
        cur.len = 0;
        xQueueSend(freeQ, &cur, 0);
        cur.data = nullptr;
        i = n;
        break;
      }
      memcpy(cur.data + cur.len, rx + i, take);
      uint32_t from = cur.len - 1, end = cur.len + take;
      uint32_t eoi = 0;
      for (uint32_t k = from; k + 1 < end; k++) {
        if (cur.data[k] == 0xFF && cur.data[k + 1] == 0xD9) {
          eoi = k + 2;
          break;
        }
      }
      if (!eoi) {
        cur.len = end;
        i = n;
        break;
      }
      i += eoi - cur.len; // Rest des Blocks gehört zum nächsten Frame
      cur.len = eoi;
      xQueueSend(fullQ, &cur, portMAX_DELAY); // der Decoder nimmt immer ab, auch alte Frames
      cur.data = nullptr;
    }
  }
  if (cur.data) xQueueSend(freeQ, &cur, 0);
}

static void netTask(void *) {
  for (;;) {
    const uint8_t src = curSource;
    const char *url = source(src).url;
    if (WiFi.status() != WL_CONNECTED) {
      streaming = false;
      setStatus("WLAN \"%s\" ...", WIFI_SSID);
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      for (int t = 0; t < 200 && WiFi.status() != WL_CONNECTED; t++) vTaskDelay(pdMS_TO_TICKS(100));
      if (WiFi.status() != WL_CONNECTED) continue;
      setStatus("WLAN ok, IP %s", WiFi.localIP().toString().c_str());
    }

    bool tls = strncmp(url, "https://", 8) == 0;
    WiFiClient plain;
    WiFiClientSecure secure;
    if (tls) secure.setInsecure(); // Demo: Zertifikat nicht prüfen
    WiFiClient &client = tls ? (WiFiClient &)secure : plain;

    HTTPClient http;
    http.useHTTP10(true); // kein Chunked-Encoding, sonst stünden Längenzeilen mitten im JPEG
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000);
    const char *keys[] = {"Content-Type"};
    http.collectHeaders(keys, 1);
    setStatus("Verbinde %s", url);
    if (!http.begin(client, url)) {
      setStatus("Ungültige URL");
      waitUnlessSwitched(src, 5000);
      continue;
    }
    int code = http.GET();
    if (curSource != src) { // während des Verbindens umgeschaltet
      http.end();
      continue;
    }
    if (code != HTTP_CODE_OK) {
      setStatus("HTTP-Fehler %d, neuer Versuch", code);
      http.end();
      waitUnlessSwitched(src, 3000);
      continue;
    }
    statConnects++;
    String type = http.header("Content-Type");
    frameUs = type.startsWith("multipart/") ? 0 : 1000000 / STREAM_FPS;
    Serial.printf("INFO verbunden: %s, %s\n", type.c_str(), frameUs ? "Datei" : "live");
    streaming = true;
    pumpStream(http.getStreamPtr(), http, src);
    http.end();
    streaming = false;
    if (curSource != src) continue;
    setStatus("Stream beendet, neu verbinden");
    waitUnlessSwitched(src, 1000);
  }
}

// ---------------------------------------------------------------------------
// Anzeige (Core 1)
// ---------------------------------------------------------------------------

// Statusleisten oben und unten (im Querformat gesehen), je BAR Pixel hoch. Das
// Video wird dort nicht gezeichnet (Bilder mit 240 Zeilen verlieren oben und
// unten je BAR Zeilen), so überschreibt kein Frame die Leisten.
static const int16_t BAR = 10;
static int16_t clipX0, clipY0, clipX1, clipY1; // sichtbarer Videobereich (exklusiv), aktuelle Rotation
static float refreshUs = 0;     // gemessene Dauer eines Panel-Refreshs
static uint32_t statLate = 0, statMaxUs = 0; // Frames, die länger als zwei Refreshes brauchten

// Anzeigemodi, seriell mit "m0".."m3" umschaltbar. Niedrige
// Refresh-Raten flimmern bei manchen Panels sichtbar, VSync braucht aber zwei
// Refreshes pro Frame (~26-30 ms). Bei 50 Hz steht jeder Frame mit 25 fps exakt
// zwei Refreshes lang, bei 75 Hz kann es knapp werden (siehe "late" in STAT).
struct Mode {
  const char *name;
  uint8_t diva, rtna;
  bool vsync;
};
static const Mode MODES[] = {
    {"VS50", 1, 0x18, true},  // ~50 Hz
    {"VS60", 1, 0x14, true},  // ~60 Hz
    {"VS75", 1, 0x10, true},  // ~75 Hz
    {"92Hz", 0, 0x18, false}, // Standard des Panels, ohne VSync (Tearing)
};
static const uint8_t NUM_MODES = sizeof(MODES) / sizeof(MODES[0]);
static uint8_t mode = 0; // VS50: am Panel verglichen sah das am besten aus
static void waitVSync();

static void applyMode(uint8_t m) {
  mode = m % NUM_MODES;
  tft.setFrameRate(MODES[mode].diva, MODES[mode].rtna);
  delay(30); // neue Rate einschwingen lassen
  waitVSync();
  uint32_t t = micros();
  for (int i = 0; i < 10; i++) waitVSync();
  refreshUs = (micros() - t) / 10.0f;
  statLate = 0;
  Serial.printf("INFO mode=%s refresh=%.1f Hz\n", MODES[mode].name, 1e6f / refreshUs);
}

static int drawMCUs(JPEGDRAW *d) {
  // Block auf den Videobereich zuschneiden (Puffer hat immer die Breite iWidth)
  int x0 = max(d->x, (int)clipX0), x1 = min(d->x + d->iWidthUsed, (int)clipX1);
  int y0 = max(d->y, (int)clipY0), y1 = min(d->y + d->iHeight, (int)clipY1);
  if (x0 >= x1 || y0 >= y1) return 1;
  const uint16_t *px = d->pPixels + (y0 - d->y) * d->iWidth + (x0 - d->x);
  if (x1 - x0 == d->iWidth) {
    tft.pushImage(x0, y0, x1 - x0, y1 - y0, px, true);
  } else { // Zeilen einzeln
    for (int y = y0; y < y1; y++, px += d->iWidth) tft.pushImage(x0, y, x1 - x0, 1, px, true);
  }
  return 1; // weiter dekodieren
}

/** WLAN-Pegel rechtsbündig bis x: "-52 dBm" und vier Balken (ab -85/-75/-65/-55 dBm). */
static void drawWifi(int16_t x, int16_t y) {
  int rssi = WiFi.RSSI();
  uint16_t on = rssi >= -65 ? ILI9341_GREEN : rssi >= -75 ? ILI9341_YELLOW : ILI9341_RED;
  for (int i = 0; i < 4; i++) {
    int16_t bh = 2 + 2 * i, bx = x - 4 * (4 - i);
    tft.fillRect(bx, y, 3, 8 - bh, ILI9341_BLACK);
    tft.fillRect(bx, y + 8 - bh, 3, bh, rssi >= -85 + 10 * i ? on : 0x2104);
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "%4d dBm ", rssi);
  tft.setCursor(x - 16 - 4 - 6 * strlen(buf), y);
  tft.print(buf);
}

/** Obere Leiste (Querformat): Nummer und Name der Quelle, rechts Art und Auflösung. */
static void drawTopBar(bool live, int w, int h) {
  tft.fillRect(0, 0, tft.width(), BAR, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.setCursor(4, 1);
  tft.printf("%u/%u %s", curSource + 1, NUM_CHOICES, source(curSource).name);
  char buf[24];
  snprintf(buf, sizeof(buf), "%s %dx%d", live ? "LIVE" : "VIDEO", w, h);
  tft.setTextColor(live ? ILI9341_RED : ILI9341_CYAN, ILI9341_BLACK);
  tft.setCursor(tft.width() - 4 - 6 * strlen(buf), 1);
  tft.print(buf);
}

/** Wartet, bis der Bildaufbau des Panels wieder oben beginnt (Scanline springt zurück). */
static void waitVSync() {
  uint16_t prev = tft.getScanline();
  uint32_t t = micros();
  while (micros() - t < 50000) {
    uint16_t l = tft.getScanline();
    if (l < prev) return;
    prev = l;
  }
}

static void showStatus() {
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print(source(curSource).name);
  tft.setTextSize(1);
  tft.setCursor(8, 28);
  tft.printf("Quelle %u/%u - BOOT: weiter", curSource + 1, NUM_CHOICES);
  tft.setTextSize(1);
  tft.setCursor(8, 40);
  tft.print(statusMsg);
}

/** Screenshot im Querformat für extras/tools/tftctl.py --shot (Befehl "P"). */
static void screenshot() {
  uint8_t rot = tft.getRotation();
  tft.setRotation(1);
  static uint16_t row[320];
  static uint8_t out[640];
  Serial.printf("SHOT %d %d\n", tft.width(), tft.height());
  for (int16_t y = 0; y < tft.height(); y++) {
    tft.readRect(0, y, tft.width(), 1, row);
    for (int16_t x = 0; x < tft.width(); x++) {
      out[2 * x] = row[x] >> 8;
      out[2 * x + 1] = row[x];
    }
    Serial.write(out, 2 * tft.width());
  }
  Serial.write('\n');
  tft.setRotation(rot);
}

void setup() {
  Serial.begin(921600);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.begin(40000000, 4); // mit WLAN nur 4 DMA-Puffer
  tft.setTextWrap(true);
  pinMode(0, INPUT_PULLUP); // BOOT-Taste: nächste Quelle
  applyMode(mode);

  if (!*WIFI_SSID) {
    setStatus("WIFI_SSID beim Bauen setzen (siehe platformio.ini)");
    showStatus();
    while (true) delay(1000);
  }

  freeQ = xQueueCreate(NUM_BUFS, sizeof(Frame));
  fullQ = xQueueCreate(NUM_BUFS, sizeof(Frame));
  for (uint8_t i = 0; i < NUM_BUFS; i++) {
    Frame f = {(uint8_t *)heap_caps_malloc(FRAME_BUF, MALLOC_CAP_8BIT), 0};
    if (!f.data) {
      setStatus("Kein RAM für Frame-Puffer");
      showStatus();
      while (true) delay(1000);
    }
    xQueueSend(freeQ, &f, 0);
  }
  Serial.printf("INFO dma_buffers=%u heap=%u maxblk=%u\n", tft.dmaBufferCount(), ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Modem-Sleep kostet viel Durchsatz
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t frames = 0, decodeUs = 0, t0 = millis(), bytes0 = 0, fps = 0, kbps = 0;
  static int16_t lastW = -1, lastH = -1;
  static bool statsDirty = false;

  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'P') screenshot();
    if (c == 'm') {
      delay(2);
      int d = Serial.read();
      if (d >= '0' && d < '0' + NUM_MODES) applyMode(d - '0'), statsDirty = true;
    }
  }
  // BOOT-Taste (entprellt): nächste Quelle. Der Netzwerk-Task bricht die
  // laufende Verbindung ab; Frames der alten Quelle werden unten verworfen.
  static bool btnPrev = true;
  static uint32_t btnT = 0;
  bool btn = digitalRead(0);
  if (!btn && btnPrev && millis() - btnT > 200) {
    btnT = millis();
    curSource = (curSource + 1) % NUM_CHOICES;
    streaming = false;
    setStatus("Verbinde ...");
    Serial.printf("INFO Quelle %u: %s\n", curSource, source(curSource).name);
  }
  btnPrev = btn;

  Frame f;
  bool got = xQueueReceive(fullQ, &f, pdMS_TO_TICKS(50)) == pdTRUE;
  if (got && f.src != curSource) { // Frame der vorherigen Quelle
    xQueueSend(freeQ, &f, 0);
    got = false;
  }
  if (got) {
    static uint32_t due = 0;
    if (frameUs) { // Datei: im Takt der Bildrate zeigen
      int32_t wait = (int32_t)(due - micros());
      if (wait > 0 && wait <= (int32_t)frameUs) {
        delay(wait / 1000); // gibt die CPU ab
        delayMicroseconds(wait % 1000);
      }
      else if (wait < -(int32_t)(4 * frameUs) || wait > (int32_t)frameUs) due = micros(); // zu weit weg: neu einrasten
      due += frameUs;
    }
    if (jpeg.openRAM(f.data, f.len, drawMCUs)) {
      // Hochformat-Frame = gedrehtes Querformat-Video: in Richtung des Bildaufbaus zeichnen
      int iw = jpeg.getWidth(), ih = jpeg.getHeight(), scale = 1;
      bool portrait = ih > iw;
      if (tft.getRotation() != (portrait ? 0 : 1)) tft.setRotation(portrait ? 0 : 1);
      const int16_t W = tft.width(), H = tft.height();
      // Größte Verkleinerung (1, 1/2, 1/4, 1/8), bei der das Bild auf den Schirm passt
      while ((iw / scale > W || ih / scale > H) && scale < 8) scale *= 2;
      int16_t ow = iw / scale, oh = ih / scale;
      int16_t ox = (W - ow) / 2, oy = (H - oh) / 2;
      if (ox < 0) ox = 0; // größer als 8x320: links oben anschneiden
      if (oy < 0) oy = 0;
      static uint8_t lastSrc = 0xFF;
      if (ow != lastW || oh != lastH || lastSrc != f.src) { // neues Format/neue Quelle: Ränder und Leisten neu
        tft.fillScreen(ILI9341_BLACK);
        lastW = ow;
        lastH = oh;
        lastSrc = f.src;
        statsDirty = true;
        if (portrait) tft.setRotation(1);
        drawTopBar(!frameUs, portrait ? oh : ow, portrait ? ow : oh);
        if (portrait) tft.setRotation(0);
        Serial.printf("INFO format %dx%d -> %dx%d (1/%d)%s\n", iw, ih, ow, oh, scale,
                      portrait ? ", gedreht, VSync" : "");
      }
      // Videobereich ohne die Leisten: im Querformat oben/unten, gedreht links/rechts
      clipX0 = portrait ? BAR : 0;
      clipX1 = portrait ? W - BAR : W;
      clipY0 = portrait ? 0 : BAR;
      clipY1 = portrait ? H : H - BAR;
      jpeg.setPixelType(RGB565_BIG_ENDIAN);
      int opt = scale == 2 ? JPEG_SCALE_HALF : scale == 4 ? JPEG_SCALE_QUARTER : scale == 8 ? JPEG_SCALE_EIGHTH : 0;
      bool vs = portrait && MODES[mode].vsync;
      if (vs) waitVSync();
      uint32_t t = micros();
      if (!jpeg.decode(ox, oy, opt)) statDropped++;
      jpeg.close();
      if (vs) { // Diagnose: muss vor dem zweiten Durchlauf des Bildaufbaus fertig sein
        tft.finishWrites();
        uint32_t us = micros() - t;
        if (us > statMaxUs) statMaxUs = us;
        if (us > 2 * refreshUs) statLate++;
      }
      frames++;
      decodeUs += micros() - t;

      // Untere Leiste: einmal pro Sekunde, Text mit Hintergrund flackert nicht
      if (statsDirty) {
        statsDirty = false;
        if (portrait) tft.setRotation(1);
        tft.setTextSize(1);
        tft.setTextColor(ILI9341_LIGHTGREY, ILI9341_BLACK);
        tft.setCursor(4, tft.height() - 9);
        tft.printf("%2lu fps  %4lu kbit/s  %s ", (unsigned long)fps, (unsigned long)kbps, vs ? MODES[mode].name : "    ");
        drawWifi(tft.width() - 4, tft.height() - 9);
        if (portrait) tft.setRotation(0);
      }
    } else {
      statDropped++;
    }
    f.len = 0;
    xQueueSend(freeQ, &f, 0);
  } else if (statusDirty && !streaming) {
    statusDirty = false;
    lastW = lastH = -1;
    showStatus();
  }

  if (millis() - t0 >= 1000) {
    uint32_t bytes = statBytes;
    kbps = (bytes - bytes0) * 8 / 1000;
    Serial.printf("STAT fps=%lu kbps=%lu dec_ms=%.1f max_ms=%.1f late=%lu mode=%s hz=%.1f drop=%lu conn=%lu heap=%u rssi=%d\n",
                  (unsigned long)frames, (unsigned long)kbps, frames ? decodeUs / 1000.0f / frames : 0.0f,
                  statMaxUs / 1000.0f, (unsigned long)statLate, MODES[mode].name, 1e6f / refreshUs, (unsigned long)statDropped,
                  (unsigned long)statConnects, ESP.getFreeHeap(), WiFi.RSSI());
    statMaxUs = 0;
    fps = frames;
    frames = decodeUs = 0;
    bytes0 = bytes;
    t0 = millis();
    statsDirty = true;
  }
}
