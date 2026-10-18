#include "ILI9341_GFX.h"

// Heiße Zeichenpfade ins IRAM: gleichbleibende Geschwindigkeit ohne
// Flash-Cache-Fehlzugriffe (gemessen: je nach Code-Lage sonst bis 25 % Schwankung)
#if ILI9341_GFX_ESP32
#define FAST_ATTR IRAM_ATTR
#else
#define FAST_ATTR
#endif

#include <algorithm>

// Klassischer 5x7-Font von Adafruit_GFX (dort static, daher hier eingebunden)
#include "glcdfont.c"

#if ILI9341_GFX_ESP32
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "hal/spi_ll.h"
#include "soc/gpio_reg.h"
#include "soc/lldesc.h"
#include "soc/soc.h"
#include "soc/spi_reg.h"
#include "soc/spi_struct.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h" // in ESP-IDF 4.4 und 5.x gleich
#endif

// Init-Sequenz: Befehl, Anzahl Datenbytes (Bit 7 = danach 150 ms warten), Daten...
static const uint8_t PROGMEM initCmds[] = {
  0xEF, 3, 0x03, 0x80, 0x02,
  0xCF, 3, 0x00, 0xC1, 0x30,              // Power control B
  0xED, 4, 0x64, 0x03, 0x12, 0x81,        // Power on sequence control
  0xE8, 3, 0x85, 0x00, 0x78,              // Driver timing control A
  0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,  // Power control A
  0xF7, 1, 0x20,                          // Pump ratio control
  0xEA, 2, 0x00, 0x00,                    // Driver timing control B
  ILI9341_PWCTR1, 1, 0x23,
  ILI9341_PWCTR2, 1, 0x10,
  ILI9341_VMCTR1, 2, 0x3E, 0x28,
  ILI9341_VMCTR2, 1, 0x86,
  ILI9341_MADCTL, 1, ILI9341_MADCTL_MX | ILI9341_MADCTL_BGR,
  ILI9341_VSCRSADD, 1, 0x00,
  ILI9341_PIXFMT, 1, 0x55,                // 16 Bit/Pixel
  ILI9341_FRMCTR1, 2, 0x00, 0x18,         // 79 Hz
  ILI9341_DFUNCTR, 3, 0x08, 0x82, 0x27,
  0xF2, 1, 0x00,                          // 3-Gamma aus
  ILI9341_GAMMASET, 1, 0x01,
  ILI9341_GMCTRP1, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
                       0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
  ILI9341_GMCTRN1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
                       0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
  ILI9341_SLPOUT, 0x80,
  ILI9341_NORON, 0,                       // Normalmodus explizit (sonst meldet 0Ah ihn nicht)
  ILI9341_DISPON, 0x80,
  0x00                                    // Ende
};

void ILI9341_GFX::invalidateAddrCache() {
  _cx0 = _cx1 = _cy0 = _cy1 = 0xFFFF;
}

void ILI9341_GFX::hwReset() {
  if (_rst >= 0) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, HIGH);
    delay(5);
    digitalWrite(_rst, LOW);
    delay(20);
    digitalWrite(_rst, HIGH);
    delay(150);
  } else {
    sendCommand(ILI9341_SWRESET);
    delay(150);
  }
}

static void runInitSequence(ILI9341_GFX &tft) {
  const uint8_t *p = initCmds;
  uint8_t cmd;
  while ((cmd = pgm_read_byte(p++)) != 0) {
    uint8_t x = pgm_read_byte(p++);
    uint8_t n = x & 0x7F;
    uint8_t buf[16];
    for (uint8_t i = 0; i < n; i++) buf[i] = pgm_read_byte(p++);
    tft.sendCommand(cmd, buf, n);
    if (x & 0x80) delay(150);
  }
}

#if ILI9341_GFX_ESP32
// ===========================================================================
// ESP32: eigener Treiber direkt auf den SPI3-Registern
//
// Alle Schreibzugriffe landen in einer Befehlsliste (Ringpuffer). Ein
// Interrupt am Ende jeder Übertragung setzt DC und startet sofort den
// nächsten Eintrag: kleine Transfers (Kommandos, Parameter) über die 64-Byte-
// Register W0..W15, große Pixelblöcke per DMA. Ist der Bus frei, gehen kleine
// Transfers direkt per Polling raus - ohne jeden Interrupt-Overhead.
// ===========================================================================

static spi_dev_t *const hw = &SPI3;

// Übertragung starten: ganzes Register schreiben (kein langsames Lesen)
static inline void IRAM_ATTR spiStart() { hw->cmd.val = SPI_USR; }
// Befehlsliste/DMA: Vollduplex wie im ESP-IDF (nötig für den RX-DMA-Workaround).
// Registerweg: Halbduplex, dann bleiben W0..W15 nach dem Senden erhalten
// (im Vollduplex überschreibt die SPI-Einheit sie mit den MISO-Bits).
static constexpr uint32_t USER_FULL = SPI_USR_MOSI | SPI_DOUTDIN;
static constexpr uint32_t USER_HALF = SPI_USR_MOSI;
static void initSpread();

static void resolvePin(int8_t pin, bool set, volatile uint32_t *&reg, uint32_t &mask) {
#if defined(GPIO_OUT1_W1TS_REG)
  if (pin >= 32) {
    reg = (volatile uint32_t *)(set ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG);
    mask = 1UL << (pin - 32);
    return;
  }
#endif
  reg = (volatile uint32_t *)(set ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG);
  mask = 1UL << pin;
}

static void IRAM_ATTR spiIsr(void *arg) { ((ILI9341_GFX *)arg)->isrHandler(); }

ILI9341_GFX::ILI9341_GFX(int8_t cs, int8_t dc, int8_t rst, int8_t sck, int8_t mosi, int8_t miso)
    : Adafruit_GFX(ILI9341_TFTWIDTH, ILI9341_TFTHEIGHT), _cs(cs), _dc(dc), _rst(rst),
      _sck(sck), _mosi(mosi), _miso(miso) {
  invalidateAddrCache();
}

inline void ILI9341_GFX::csLow()  { if (_cs >= 0) *_csClr = _csMask; }
inline void ILI9341_GFX::csHigh() { if (_cs >= 0) *_csSet = _csMask; }

static uint32_t calcClock(uint32_t freq) {
  spi_ll_clock_val_t v;
  spi_ll_master_cal_clock(APB_CLK_FREQ, freq, 128, &v);
  return v;
}

void ILI9341_GFX::begin(uint32_t freq, uint8_t dmaBuffers) {
  _freq = freq;

  if (_cs >= 0) {
    pinMode(_cs, OUTPUT);
    digitalWrite(_cs, HIGH);
    uint32_t m;
    resolvePin(_cs, true, _csSet, _csMask);
    resolvePin(_cs, false, _csClr, m);
  }
  pinMode(_dc, OUTPUT);
  digitalWrite(_dc, HIGH);
  resolvePin(_dc, true, _dcSet, _dcMask);
  resolvePin(_dc, false, _dcClr, _dcMask);

  // ESP-IDF nur für Pins (IOMUX), Peripherie-Takt und DMA-Kanal
  spi_bus_config_t bus = {};
  bus.mosi_io_num = _mosi;
  bus.miso_io_num = _miso;
  bus.sclk_io_num = _sck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = BUF_BYTES;
  bus.flags = SPICOMMON_BUSFLAG_MASTER;
  spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH1);
  if (_miso >= 0) {
    // MISO über die GPIO-Matrix statt IOMUX: im Dual-Modus treibt die
    // SPI-Einheit die Q-Leitung, die dann nur am DC-Pin ankommen soll
    esp_rom_gpio_pad_select_gpio(_miso);
    gpio_set_direction((gpio_num_t)_miso, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal(_miso, VSPIQ_IN_IDX, false);
  }

  if (!_desc) _desc = (lldesc_t *)heap_caps_calloc(MAX_BUFS * 2, sizeof(lldesc_t), MALLOC_CAP_DMA);
  dmaBuffers = constrain(dmaBuffers, 2, MAX_BUFS);
  while (_numBufs < dmaBuffers) {
    uint8_t *b = (uint8_t *)heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA);
    if (!b) break;
    _buf[_numBufs++] = b;
  }

  _clkWrite = calcClock(_freq);
  _clkRead = calcClock(10000000); // Lesen ist nur langsam zuverlässig
  configureHw();
  if (!_intr) esp_intr_alloc(ETS_SPI3_INTR_SOURCE, ESP_INTR_FLAG_IRAM, spiIsr, this, &_intr);

  hwReset();
  runInitSequence(*this);

  _width = ILI9341_TFTWIDTH;
  _height = ILI9341_TFTHEIGHT;
  rotation = 0;
  invalidateAddrCache();
  initSpread();
  setPacked(true);
}

void ILI9341_GFX::releaseDmaBuffers(uint8_t keep) {
  if (keep < 2) keep = 2;
  finishWrites();
  while (_numBufs > keep) {
    _numBufs--;
    heap_caps_free(_buf[_numBufs]);
    _buf[_numBufs] = nullptr;
    _bufSeq[_numBufs] = 0;
  }
  _bufIdx = 0;
}

void ILI9341_GFX::configureHw() {
  spi_ll_master_init(hw);
  hw->user.val = 0;
  hw->user.usr_mosi = 1;
  hw->user.doutdin = 1; // Vollduplex wie im ESP-IDF, MISO-Phase bleibt aus
  hw->ctrl.val = 0;      // MSB zuerst
  spi_ll_master_set_mode(hw, 0);
  hw->pin.cs0_dis = 1;   // CS steuern wir selbst
  hw->pin.cs1_dis = 1;
  hw->pin.cs2_dis = 1;
  hw->clock.val = _clkWrite;
  hw->slave.trans_done = 0;
  hw->slave.trans_inten = 1;
}

void ILI9341_GFX::setSPISpeed(uint32_t freq) {
  _freq = freq;
  _clkWrite = calcClock(freq);
  if (!_desc) return; // begin() noch nicht aufgerufen
  finishWrites();
  hw->clock.val = _clkWrite;
}

// ---- Befehlsliste ----------------------------------------------------------

void IRAM_ATTR ILI9341_GFX::startOp(const Op &op) {
  if (op.buf == PACKED_OP) {
    startPacked(op);
    return;
  }
  if (_dual) dualOff(_intOn ? USER_FULL : USER_HALF);
  if (op.dc) *_dcSet = _dcMask;
  else *_dcClr = _dcMask;

  if (op.buf == 0xFF) {
    // DMA-Ausgang abschalten, sonst holt die SPI-Einheit die Daten weiter
    // vom (beendeten) DMA statt aus den Registern
    if (_dmaDirty) {
      hw->dma_out_link.val = 0;
      spi_dma_ll_tx_reset(hw, 0);
      spi_ll_dma_tx_fifo_reset(hw);
      _dmaDirty = false;
    }
    for (uint32_t i = 0; i < (op.len + 3u) / 4; i++) hw->data_buf[i] = op.data[i];
  } else {
    lldesc_t *d = &_desc[op.buf * 2];
    uint8_t *b = _buf[op.buf];
    uint32_t l0 = op.len > 4092 ? 4092 : op.len, l1 = op.len - l0;
    d[0].size = (l0 + 3) & ~3u;
    d[0].length = l0;
    d[0].offset = 0;
    d[0].sosf = 0;
    d[0].eof = l1 == 0;
    d[0].owner = 1;
    d[0].buf = b;
    d[0].qe.stqe_next = l1 ? &d[1] : nullptr;
    if (l1) {
      d[1].size = (l1 + 3) & ~3u;
      d[1].length = l1;
      d[1].offset = 0;
      d[1].sosf = 0;
      d[1].eof = 1;
      d[1].owner = 1;
      d[1].buf = b + l0;
      d[1].qe.stqe_next = nullptr;
    }
    _dmaDirty = true;
    // Reihenfolge wie im ESP-IDF: usr_mosi erst nach dem DMA-Start setzen
    hw->user.usr_mosi = 0;
    spi_dma_ll_tx_reset(hw, 0);
    spi_ll_dma_tx_fifo_reset(hw);
    spi_dma_ll_tx_start(hw, 0, d);
    // Workaround aus dem ESP-IDF für ESP32-Silizium: RX-DMA pro forma starten,
    // sonst geht das erste Byte verloren
    hw->dma_in_link.addr = 0;
    hw->dma_in_link.start = 1;
    hw->user.usr_mosi = 1;
    hw->mosi_dlen.val = op.len * 8 - 1;
    // Bewusst Lesen-Ändern-Schreiben: der Lesezugriff gibt dem DMA die Zeit,
    // das erste Wort zu holen (sonst ist bei 40 MHz das erste Byte falsch)
    hw->cmd.usr = 1;
    return;
  }
  hw->mosi_dlen.val = op.len * 8 - 1;
  spiStart(); // ganzes Register schreiben: kein langsames Lesen
}

void IRAM_ATTR ILI9341_GFX::isrHandler() {
  // Robust gegen falsche Aufrufe: nur eine echte Fertigmeldung einer
  // Übertragung aus der Befehlsliste darf gezählt werden.
  if (!hw->slave.trans_done) return; // verspäteter Doppelaufruf
  hw->slave.trans_done = 0;           // erst löschen, dann prüfen: so geht nichts verloren
  if (hw->cmd.val & SPI_USR) return;  // läuft noch: verspätetes Flag einer früheren Übertragung
  if (!_busy) return;                 // nichts in der Liste (z.B. direkte Übertragung)
  _doneSeq = _doneSeq + 1;
  _pendingBytes = _pendingBytes - _ops[_opTail % NUM_OPS].len;
  uint32_t tail = _opTail + 1;
  _opTail = tail;
  if (tail != _opHead) startOp(_ops[tail % NUM_OPS]);
  else _busy = false;
}

FAST_ATTR uint32_t ILI9341_GFX::submit(const Op &op) {
  while (_opHead - _opTail >= NUM_OPS) {
  } // Liste voll: warten, bis der Interrupt Platz schafft

  uint32_t seq;
  if (!_busy && op.buf == 0xFF) {
    // Befehlsliste leer und kleiner Transfer: direkt über die Register. Ohne
    // laufende Liste kann kein Interrupt dazwischenfunken.
    enterDirect();
    while (hw->cmd.val & SPI_USR) {
    }
    startOp(op);
    _submitted = _submitted + 1; // kein ++ auf volatile (C++20)
    seq = _submitted;
    _doneSeq = seq;
    return seq;
  }
  portENTER_CRITICAL(&_mux);
  {
    if (!_busy) leaveDirect();
    _ops[_opHead % NUM_OPS] = op;
    _pendingBytes = _pendingBytes + op.len;
    _opHead = _opHead + 1;
    _submitted = _submitted + 1; // kein ++ auf volatile (C++20)
    seq = _submitted;
    if (!_busy) {
      _busy = true;
      startOp(_ops[_opTail % NUM_OPS]);
    }
  }
  portEXIT_CRITICAL(&_mux);
  return seq;
}

// Wechsel in den Registerweg. DMA kann nur über die Befehlsliste gelaufen
// sein, also werden seine Reste genau hier einmal beseitigt.
FAST_ATTR void ILI9341_GFX::enterDirect() {
  if (_intOn) {
    hw->slave.trans_inten = 0;
    _intOn = false;
    if (_dmaDirty) {
      while (hw->cmd.val & SPI_USR) {
      }
      hw->dma_out_link.val = 0;
      spi_dma_ll_tx_reset(hw, 0);
      spi_ll_dma_tx_fifo_reset(hw);
      _dmaDirty = false;
    }
    while (hw->cmd.val & SPI_USR) {
    }
    if (_dual) dualOff(USER_HALF); // letzte Listen-Übertragung war gepackt
    else hw->user.val = USER_HALF;
  }
}

FAST_ATTR void ILI9341_GFX::leaveDirect() {
  if (!_intOn) {
    while (hw->cmd.val & SPI_USR) {
    }
    if (_dual) dualOff(USER_FULL);
    else hw->user.val = USER_FULL;
    // Die Hardware setzt trans_done ein paar Takte NACH dem Löschen von
    // SPI_USR. Ohne diese Pause würde das späte Flag der letzten direkten
    // Übertragung einen falschen Interrupt auslösen und die Zählung der
    // Befehlsliste verschieben (gemessen: Hänger mit tail > head).
    delayMicroseconds(1);
    hw->slave.trans_done = 0;
    hw->slave.trans_inten = 1;
    _intOn = true;
  }
}

// Vorherige Übertragung abwarten, DC setzen (nur nach enterDirect())
FAST_ATTR void ILI9341_GFX::directBegin(bool dc) {
  if (_dual) leaveDual();
  while (hw->cmd.val & SPI_USR) {
  }
  if (dc) *_dcSet = _dcMask;
  else *_dcClr = _dcMask;
}

FAST_ATTR void ILI9341_GFX::directWrite(bool dc, uint32_t w0, uint32_t bits) {
  directBegin(dc);
  hw->mosi_dlen.val = bits - 1;
  hw->data_buf[0] = w0;
  spiStart();
}

// Füllen ohne DMA (wie LovyanGFX): Muster einmal in W0..W15 laden und die
// Übertragung mehrfach starten (Halbduplex: die Register bleiben erhalten)
FAST_ATTR void ILI9341_GFX::directFill(uint16_t swapped, uint32_t len) {
  directBegin(true);
  const uint32_t pat = swapped | ((uint32_t)swapped << 16);
  uint32_t chunk = len > 32 ? 32 : len;
  for (uint32_t i = 0; i < (chunk + 1) / 2; i++) hw->data_buf[i] = pat;
  hw->mosi_dlen.val = chunk * 16 - 1;
  spiStart();
  len -= chunk;
  while (len) {
    uint32_t c = len > 32 ? 32 : len;
    while (hw->cmd.val & SPI_USR) {
    }
    if (c != chunk) {
      hw->mosi_dlen.val = c * 16 - 1;
      chunk = c;
    }
    spiStart();
    len -= c;
  }
}

// Adressfenster direkt über die Register (nur bei leerer Befehlsliste)
FAST_ATTR void ILI9341_GFX::directWindow(uint16_t x, uint16_t y, uint16_t x1, uint16_t y1) {
  enterDirect();
  if (x != _cx0 || x1 != _cx1) {
    directWrite(false, ILI9341_CASET, 8);
    directWrite(true, (x >> 8) | ((x & 0xFF) << 8) | ((uint32_t)(x1 >> 8) << 16) | ((uint32_t)(x1 & 0xFF) << 24), 32);
    _cx0 = x;
    _cx1 = x1;
  }
  if (y != _cy0 || y1 != _cy1) {
    directWrite(false, ILI9341_PASET, 8);
    directWrite(true, (y >> 8) | ((y & 0xFF) << 8) | ((uint32_t)(y1 >> 8) << 16) | ((uint32_t)(y1 & 0xFF) << 24), 32);
    _cy0 = y;
    _cy1 = y1;
  }
  directWrite(false, ILI9341_RAMWR, 8);
}

// Gepackt: Fenster, RAMWR und die ersten Pixel als EINE Übertragung. Dafür
// läuft die SPI-Einheit im Dual-Modus (2 Bit je Takt): MOSI trägt die Daten,
// die zweite Leitung (Q) liegt über die GPIO-Matrix auf dem DC-Pin und trägt
// zu jedem Bit den passenden D/CX-Pegel. Taktgenau in Hardware, ohne
// Zeitabhängigkeit. Jedes Byte wird dazu auf 16 Pufferbits gespreizt.
static uint16_t s_spread[256]; // Datenbits auf den MOSI-Positionen
static uint16_t s_dcBits;      // DC=1 auf allen Q-Positionen

// Bitbelegung im Dual-Modus (per Readback bestätigt): von jedem Bitpaar geht
// das höhere Bit auf Q (hier DC), das niedrigere auf MOSI
static void initSpread() {
  for (uint32_t b = 0; b < 256; b++) {
    uint16_t v = 0;
    for (uint32_t i = 0; i < 8; i++) {
      // Datenbit i -> Paar 7-i; 4 Paare je Pufferbyte, Puffer geht byteweise MSB zuerst raus
      uint32_t pair = 7 - i;
      uint32_t pos = (pair / 4) * 8 + 6 - (pair % 4) * 2;
      if ((b >> i) & 1) v |= 1u << pos;
    }
    s_spread[b] = v;
  }
  s_dcBits = 0xAAAA;
}

FAST_ATTR void ILI9341_GFX::dualOn() {
  hw->user.val = SPI_USR_MOSI | SPI_FWRITE_DUAL;
  hw->ctrl.val = SPI_FASTRD_MODE;
  GPIO.func_out_sel_cfg[_dc].val = VSPIQ_OUT_IDX | (1 << 10); // OE weiter aus GPIO_ENABLE
  _dual = true;
}

// Nur bei ruhender SPI-Einheit; user: Registerwert für den folgenden Betrieb
FAST_ATTR void ILI9341_GFX::dualOff(uint32_t user) {
  GPIO.func_out_sel_cfg[_dc].val = SIG_GPIO_OUT_IDX;
  hw->ctrl.val = 0;
  hw->user.val = user;
  _dual = false;
}

FAST_ATTR void ILI9341_GFX::leaveDual() {
  while (hw->cmd.val & SPI_USR) {
  }
  dualOff(USER_HALF);
}

// Gespreizte Halbwörter für [CASET x x1] [PASET y y1] RAMWR + m Pixel
// (hdr Bit 0/1: CASET/PASET nötig). Rückgabe: Anzahl Halbwörter (= Bytes auf der Leitung).
static inline uint32_t IRAM_ATTR buildPacked(uint16_t *h, uint32_t hdr, uint16_t x, uint16_t x1,
                                             uint16_t y, uint16_t y1, uint16_t color, uint32_t m) {
  const uint16_t *sp = s_spread;
  const uint16_t dcb = s_dcBits;
  uint32_t k = 0;
  if (hdr & 1) {
    h[0] = sp[ILI9341_CASET];
    h[1] = sp[x >> 8] | dcb;
    h[2] = sp[x & 0xFF] | dcb;
    h[3] = sp[x1 >> 8] | dcb;
    h[4] = sp[x1 & 0xFF] | dcb;
    k = 5;
  }
  if (hdr & 2) {
    h[k] = sp[ILI9341_PASET];
    h[k + 1] = sp[y >> 8] | dcb;
    h[k + 2] = sp[y & 0xFF] | dcb;
    h[k + 3] = sp[y1 >> 8] | dcb;
    h[k + 4] = sp[y1 & 0xFF] | dcb;
    k += 5;
  }
  h[k++] = sp[ILI9341_RAMWR];
  const uint16_t ph = sp[color >> 8] | dcb, pl = sp[color & 0xFF] | dcb;
  for (uint32_t i = 0; i < m; i++) {
    h[k++] = ph;
    h[k++] = pl;
  }
  return k;
}

// Kompakte Form einer gepackten Strecke in der Befehlsliste (Op::data)
struct PackedOp {
  uint16_t x, x1, y, y1, color;
  uint8_t n, hdr;
};

// Gepackte Strecke als ein Eintrag der Befehlsliste: die Interrupt-Routine
// spreizt sie selbst (statt bis zu sechs Einträgen mit je einem Interrupt)
FAST_ATTR uint32_t ILI9341_GFX::queuePacked(uint16_t x, uint16_t y, uint16_t x1, uint16_t y1,
                                            uint16_t color, uint32_t n) {
  Op op;
  op.buf = PACKED_OP;
  op.dc = 1;
  PackedOp *p = (PackedOp *)op.data;
  uint8_t hdr = 0;
  if (x != _cx0 || x1 != _cx1) {
    hdr |= 1;
    _cx0 = x;
    _cx1 = x1;
  }
  if (y != _cy0 || y1 != _cy1) {
    hdr |= 2;
    _cy0 = y;
    _cy1 = y1;
  }
  p->x = x;
  p->x1 = x1;
  p->y = y;
  p->y1 = y1;
  const uint32_t k = ((hdr & 1) ? 5 : 0) + ((hdr & 2) ? 5 : 0) + 1;
  uint32_t m = (32 - k) / 2;
  if (m > n) m = n;
  p->color = color;
  p->n = m;
  p->hdr = hdr;
  op.len = k + 2 * m;
  submit(op);
  return m;
}

void IRAM_ATTR ILI9341_GFX::startPacked(const Op &op) {
  const PackedOp *p = (const PackedOp *)op.data;
  union {
    uint32_t w[16];
    uint16_t h[32];
  } u;
  uint32_t k = buildPacked(u.h, p->hdr, p->x, p->x1, p->y, p->y1, p->color, p->n);
  if (!_dual) dualOn();
  for (uint32_t i = 0; i < (k + 1) / 2; i++) hw->data_buf[i] = u.w[i];
  hw->mosi_dlen.val = k * 16 - 1;
  spiStart();
}

FAST_ATTR void ILI9341_GFX::packedFill(uint16_t x, uint16_t y, uint16_t x1, uint16_t y1,
                                       uint16_t color, uint32_t n) {
  enterDirect();
  uint32_t hdr = 0, k = 1;
  if (x != _cx0 || x1 != _cx1) {
    hdr |= 1;
    k += 5;
    _cx0 = x;
    _cx1 = x1;
  }
  if (y != _cy0 || y1 != _cy1) {
    hdr |= 2;
    k += 5;
    _cy0 = y;
    _cy1 = y1;
  }
  uint32_t m = (32 - k) / 2;
  if (m > n) m = n;
  union {
    uint32_t w[16];
    uint16_t h[32];
  } u;
  k = buildPacked(u.h, hdr, x, x1, y, y1, color, m);
  n -= m;

  while (hw->cmd.val & SPI_USR) {
  }
  if (!_dual) dualOn();
  for (uint32_t i = 0; i < (k + 1) / 2; i++) hw->data_buf[i] = u.w[i];
  hw->mosi_dlen.val = k * 16 - 1;
  spiStart();

  if (!n) return;
  if (n <= 16) { // Rest passt in eine weitere Dual-Übertragung
    const uint32_t pw = u.h[k - 2] | ((uint32_t)u.h[k - 1] << 16);
    while (hw->cmd.val & SPI_USR) {
    }
    for (uint32_t i = 0; i < n; i++) hw->data_buf[i] = pw;
    hw->mosi_dlen.val = n * 32 - 1;
    spiStart();
    return;
  }
  leaveDual();
  directFill((color << 8) | (color >> 8), n);
}

FAST_ATTR uint32_t ILI9341_GFX::submitSmall(const uint8_t *data, uint32_t len, bool dc) {
  Op op;
  op.len = len;
  op.dc = dc;
  op.buf = 0xFF;
  memcpy(op.data, data, len);
  return submit(op);
}

FAST_ATTR void ILI9341_GFX::waitFor(uint32_t seq) {
  while ((int32_t)(_doneSeq - seq) < 0) {
  }
}

// ---- Lesen und synchrones Schreiben (nur bei leerer Befehlsliste) ----------

void ILI9341_GFX::pollWrite(const uint8_t *data, uint32_t len, bool dc) {
  Op op;
  op.len = len;
  op.dc = dc;
  op.buf = 0xFF;
  memcpy(op.data, data, len);
  enterDirect();
  if (_dual) leaveDual();
  while (hw->cmd.val & SPI_USR) {
  }
  startOp(op);
  while (hw->cmd.val & SPI_USR) {
  }
}

void ILI9341_GFX::pollRead(uint8_t *buf, uint32_t len) {
  // Vollduplex wie im ESP-IDF: Nullen senden, gleichzeitig empfangen
  enterDirect();
  directBegin(true);
  hw->user.val = USER_FULL | SPI_USR_MISO;
  while (len) {
    uint32_t n = len > 64 ? 64 : len;
    for (uint32_t i = 0; i < (n + 3) / 4; i++) hw->data_buf[i] = 0;
    hw->mosi_dlen.val = n * 8 - 1;
    hw->miso_dlen.val = n * 8 - 1;
    spiStart();
    while (hw->cmd.val & SPI_USR) {
    }
    spi_ll_read_buffer(hw, buf, n * 8);
    buf += n;
    len -= n;
  }
  hw->user.val = USER_HALF;
}

// ---- Puffer und öffentliche Schreibfunktionen -------------------------------

FAST_ATTR void ILI9341_GFX::startWrite() {
  if (_txDepth++ == 0 && !_busHeld) {
    csLow();
    _busHeld = true;
  }
}

FAST_ATTR void ILI9341_GFX::endWrite() {
  // Übertragung läuft im Hintergrund weiter, CS bleibt aktiv
  if (_txDepth && --_txDepth == 0) flushBuffer();
}

void ILI9341_GFX::finishWrites() {
  if (!_busHeld || _txDepth) return;
  flushBuffer();
  waitIdle();
  while (hw->cmd.val & SPI_USR) { // letzte direkte Übertragung
  }
  csHigh();
  _busHeld = false;
}

FAST_ATTR uint8_t *ILI9341_GFX::acquireBuffer() {
  waitFor(_bufSeq[_bufIdx]);
  return _buf[_bufIdx];
}

FAST_ATTR void ILI9341_GFX::flushBuffer() {
  if (!_bufFill) return;
  if (_bufFill <= INLINE_BYTES) {
    // Klein genug für die Register: Puffer bleibt sofort frei
    submitSmall(_buf[_bufIdx], _bufFill, true);
  } else {
    Op op;
    op.len = _bufFill;
    op.dc = 1;
    op.buf = _bufIdx;
    _bufSeq[_bufIdx] = submit(op);
    _bufIdx = (_bufIdx + 1) % _numBufs;
  }
  _bufFill = 0;
}

FAST_ATTR void ILI9341_GFX::appendBytes(const uint8_t *data, uint32_t len) {
  while (len) {
    uint8_t *buf = _bufFill ? _buf[_bufIdx] : acquireBuffer();
    uint32_t n = min(len, BUF_BYTES - _bufFill);
    memcpy(buf + _bufFill, data, n);
    _bufFill += n;
    data += n;
    len -= n;
    if (_bufFill == BUF_BYTES) flushBuffer();
  }
}

FAST_ATTR void ILI9341_GFX::writeCommand(uint8_t cmd) {
  flushBuffer();
  if (directOk()) {
    enterDirect();
    directWrite(false, cmd, 8);
    return;
  }
  submitSmall(&cmd, 1, false);
}

void ILI9341_GFX::spiWrite(uint8_t b) {
  if (directOk()) {
    enterDirect();
    directWrite(true, b, 8);
    return;
  }
  appendBytes(&b, 1);
}

FAST_ATTR void ILI9341_GFX::spiWrite16(uint16_t w) {
  if (directOk()) {
    enterDirect();
    directWrite(true, (w >> 8) | ((w & 0xFF) << 8), 16);
    return;
  }
  uint8_t b[2] = {(uint8_t)(w >> 8), (uint8_t)w};
  appendBytes(b, 2);
}

void ILI9341_GFX::writeAddrCmd(uint8_t cmd, uint16_t a, uint16_t b) {
  writeCommand(cmd);
  if (directOk()) { // beide Parameter in einer 32-Bit-Übertragung
    directWrite(true, (a >> 8) | ((a & 0xFF) << 8) | ((uint32_t)(b >> 8) << 16) | ((uint32_t)(b & 0xFF) << 24), 32);
    return;
  }
  spiWrite16(a);
  spiWrite16(b);
}

FAST_ATTR void ILI9341_GFX::writePixels(const uint16_t *colors, uint32_t len, bool bigEndian) {
  if (bigEndian) {
    appendBytes((const uint8_t *)colors, len * 2);
    return;
  }
  if (len <= 32 && directOk()) { // kurz: direkt über die Register
    enterDirect();
    directBegin(true);
    for (uint32_t i = 0; i < len; i += 2) {
      uint16_t a = colors[i], b = i + 1 < len ? colors[i + 1] : 0;
      hw->data_buf[i / 2] = (uint16_t)((a >> 8) | (a << 8)) | ((uint32_t)(uint16_t)((b >> 8) | (b << 8)) << 16);
    }
    hw->mosi_dlen.val = len * 16 - 1;
    spiStart();
    return;
  }
  if (_bufFill & 1) flushBuffer(); // 16-Bit-Zugriffe brauchen gerade Adressen
  while (len) {
    uint8_t *buf = _bufFill ? _buf[_bufIdx] : acquireBuffer();
    uint32_t n = min(len, (BUF_BYTES - _bufFill) / 2);
    uint16_t *dst = (uint16_t *)(buf + _bufFill);
    const uint16_t *src = colors;
    uint32_t i = n;
    if (!(((uintptr_t)dst | (uintptr_t)src) & 3)) {
      // zwei Pixel pro Wort tauschen
      const uint32_t *s = (const uint32_t *)src;
      uint32_t *d = (uint32_t *)dst;
      for (uint32_t k = i >> 1; k; k--) {
        uint32_t v = *s++;
        *d++ = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
      }
      src = (const uint16_t *)s;
      dst = (uint16_t *)d;
      i &= 1;
    }
    while (i--) {
      uint16_t v = *src++;
      *dst++ = (v << 8) | (v >> 8);
    }
    _bufFill += n * 2;
    colors += n;
    len -= n;
    if (_bufFill >= BUF_BYTES - 1) flushBuffer();
  }
}

FAST_ATTR void ILI9341_GFX::writeColor(uint16_t color, uint32_t len) {
  if (!len) return;
  flushBuffer();
  uint16_t swapped = (color << 8) | (color >> 8);
  if (len <= 2048 && directOk()) { // bis ~2000 Pixel ohne DMA (kein Interrupt-Overhead)
    enterDirect();
    directFill(swapped, len);
    return;
  }
  if (len * 2 <= INLINE_BYTES) {
    uint16_t px[INLINE_BYTES / 2];
    for (uint32_t i = 0; i < len; i++) px[i] = swapped;
    submitSmall((const uint8_t *)px, len * 2, true);
    return;
  }
  uint16_t *buf = (uint16_t *)acquireBuffer();
  uint32_t n = min(len, BUF_BYTES / 2);
  for (uint32_t i = 0; i < n; i++) buf[i] = swapped;
  // Derselbe Puffer wird mehrfach gesendet
  while (len) {
    Op op;
    op.len = min(len, n) * 2;
    op.dc = 1;
    op.buf = _bufIdx;
    _bufSeq[_bufIdx] = submit(op);
    len -= op.len / 2;
  }
  _bufIdx = (_bufIdx + 1) % _numBufs;
}

uint16_t *ILI9341_GFX::pixelSpace(uint32_t maxPx, uint32_t &n) {
  if (_bufFill & 1) flushBuffer();
  uint8_t *buf = _bufFill ? _buf[_bufIdx] : acquireBuffer();
  n = min(maxPx, (BUF_BYTES - _bufFill) / 2);
  return (uint16_t *)(buf + _bufFill);
}

void ILI9341_GFX::commitPixels(uint32_t n) {
  _bufFill += n * 2;
  if (_bufFill >= BUF_BYTES - 1) flushBuffer();
}

void ILI9341_GFX::setFrameRate(uint8_t diva, uint8_t rtna) {
  uint8_t p[2] = {(uint8_t)(diva & 3), (uint8_t)(rtna & 0x1F)};
  sendCommand(ILI9341_FRMCTR1, p, 2);
}

void ILI9341_GFX::readRegister(uint8_t cmd, uint8_t *buf, uint8_t len) {
  finishWrites();
  csLow();
  hw->clock.val = _clkRead;
  pollWrite(&cmd, 1, false);
  pollRead(buf, len);
  hw->clock.val = _clkWrite;
  csHigh();
}

uint8_t ILI9341_GFX::readcommand8(uint8_t cmd, uint8_t index) {
  // Undokumentierter Befehl 0xD9 wählt das zu lesende Parameterbyte
  uint8_t idx = 0x10 + index;
  sendCommand(0xD9, &idx, 1);
  uint8_t r;
  readRegister(cmd, &r, 1);
  return r;
}

uint16_t ILI9341_GFX::getScanline() {
  // Über 4-Draht-SPI kommt statt des Dummy-Bytes nur ein Dummy-Takt (gemessen):
  // 1 Bit Dummy, dann GTS[15:8], GTS[7:0]
  uint8_t b[3];
  readRegister(0x45, b, 3);
  uint32_t raw = ((uint32_t)b[0] << 16) | (b[1] << 8) | b[2];
  return (raw >> 7) & 0x3FF;
}

// Prüft den gepackten Modus per Readback: zufällige kurze Strecken in einen
// 32x32-Bereich bei (112,112). Koordinaten (112..143) und Farbbytes
// (0x70..0xAF) sind so gewählt, dass ein falsch als Befehl erkanntes Byte nie
// ein gültiges ILI9341-Kommando ist. Rückgabe: Anzahl falscher Pixel.
uint32_t ILI9341_GFX::verifyPacked(uint32_t seed) {
  const int16_t X = 112, Y = 112, S = 32;
  uint16_t *want = (uint16_t *)malloc(S * S * sizeof(uint16_t)); // 2 KB
  if (!want) return UINT32_MAX; // nicht testbar (kein RAM)
  bool keep = _packed;
  _packed = false;
  // Hintergrund über die Befehlsliste (DMA): prüft auch den Wechsel DMA -> Dual
  for (int i = 0; i < S * S; i++) want[i] = 0x7070;
  startWrite();
  setAddrWindow(X, Y, S, S);
  writePixels(want, S * S);
  endWrite();
  _packed = true;
  startWrite();
  for (int i = 0; i < 200; i++) {
    seed = seed * 1664525u + 1013904223u;
    int16_t x = X + (seed >> 8) % S, y = Y + (seed >> 14) % S;
    int16_t len = 1 + (seed >> 20) % 40; // bis 40: auch der Restpfad nach dem Dual-Paket
    uint16_t c = ((0x70 + ((seed >> 2) & 0x3F)) << 8) | (0x70 + ((seed >> 26) & 0x3F));
    bool horiz = seed & 1;
    int16_t room = horiz ? X + S - x : Y + S - y;
    if (len > room) len = room;
    writeFillRect(x, y, horiz ? len : 1, horiz ? 1 : len, c);
    for (int k = 0; k < len; k++)
      want[(y - Y + (horiz ? 0 : k)) * S + (x - X + (horiz ? k : 0))] = c;
  }
  endWrite();
  _packed = keep;
  uint16_t got[S]; // zeilenweise zurücklesen: nur 64 Byte Stack
  uint32_t bad = 0;
  for (int16_t r = 0; r < S; r++) {
    readRect(X, Y + r, S, 1, got);
    for (int i = 0; i < S; i++) bad += got[i] != want[r * S + i];
  }
  free(want);
  return bad;
}

void ILI9341_GFX::setPacked(bool on) {
  finishWrites();
  _packed = on && _dc >= 0 && _dc < 34;
}

void ILI9341_GFX::reinitPanel() {
  finishWrites();
  hwReset();
  runInitSequence(*this);
  setRotation(rotation);
}

void ILI9341_GFX::readRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *out) {
  finishWrites();
  csLow();
  uint8_t c = ILI9341_CASET;
  uint8_t p[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
  pollWrite(&c, 1, false);
  pollWrite(p, 4, true);
  c = ILI9341_PASET;
  uint8_t q[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1)};
  pollWrite(&c, 1, false);
  pollWrite(q, 4, true);

  hw->clock.val = _clkRead;
  c = ILI9341_RAMRD;
  pollWrite(&c, 1, false);
  // Erst ein Dummy-Byte, dann je Pixel 3 Byte (RGB666, linksbündig)
  uint8_t rx[63];
  pollRead(rx, 1);
  uint32_t n = (uint32_t)w * h;
  while (n) {
    uint32_t k = n > 21 ? 21 : n;
    pollRead(rx, k * 3);
    for (uint32_t i = 0; i < k; i++)
      *out++ = ((rx[i * 3] & 0xF8) << 8) | ((rx[i * 3 + 1] & 0xFC) << 3) | (rx[i * 3 + 2] >> 3);
    n -= k;
  }
  hw->clock.val = _clkWrite;
  csHigh();
  invalidateAddrCache();
}

#else
// ===========================================================================
// Andere Plattformen: Arduino-SPI
// ===========================================================================

ILI9341_GFX::ILI9341_GFX(int8_t cs, int8_t dc, int8_t rst, SPIClass *spi)
    : Adafruit_GFX(ILI9341_TFTWIDTH, ILI9341_TFTHEIGHT), _cs(cs), _dc(dc), _rst(rst),
      _spi(spi), _settings(ILI9341_GFX_DEFAULT_FREQ, MSBFIRST, SPI_MODE0) {
  invalidateAddrCache();
}

inline void ILI9341_GFX::csLow()  { if (_cs >= 0) digitalWrite(_cs, LOW); }
inline void ILI9341_GFX::csHigh() { if (_cs >= 0) digitalWrite(_cs, HIGH); }
inline void ILI9341_GFX::dcLow()  { digitalWrite(_dc, LOW); }
inline void ILI9341_GFX::dcHigh() { digitalWrite(_dc, HIGH); }

void ILI9341_GFX::begin(uint32_t freq) {
  setSPISpeed(freq);
  if (_cs >= 0) {
    pinMode(_cs, OUTPUT);
    digitalWrite(_cs, HIGH);
  }
  pinMode(_dc, OUTPUT);
  digitalWrite(_dc, HIGH);
  _spi->begin();

  hwReset();
  runInitSequence(*this);

  _width = ILI9341_TFTWIDTH;
  _height = ILI9341_TFTHEIGHT;
  rotation = 0;
  invalidateAddrCache();
}

void ILI9341_GFX::setSPISpeed(uint32_t freq) {
  _freq = freq;
  _settings = SPISettings(freq, MSBFIRST, SPI_MODE0);
}

void ILI9341_GFX::startWrite() {
  if (_txDepth++ == 0) {
    _spi->beginTransaction(_settings);
    csLow();
  }
}

void ILI9341_GFX::endWrite() {
  if (_txDepth && --_txDepth == 0) {
    csHigh();
    _spi->endTransaction();
  }
}

void ILI9341_GFX::spiWrite(uint8_t b) { _spi->transfer(b); }
void ILI9341_GFX::spiWrite16(uint16_t w) { _spi->transfer16(w); }

void ILI9341_GFX::writeCommand(uint8_t cmd) {
  dcLow();
  _spi->transfer(cmd);
  dcHigh();
}

void ILI9341_GFX::writePixels(const uint16_t *colors, uint32_t len, bool bigEndian) {
  if (bigEndian) {
    const uint8_t *b = (const uint8_t *)colors;
    for (uint32_t i = 0; i < len * 2; i++) _spi->transfer(b[i]);
  } else {
    while (len--) _spi->transfer16(*colors++);
  }
}

void ILI9341_GFX::writeColor(uint16_t color, uint32_t len) {
  while (len--) _spi->transfer16(color);
}

void ILI9341_GFX::writeAddrCmd(uint8_t cmd, uint16_t a, uint16_t b) {
  writeCommand(cmd);
  spiWrite16(a);
  spiWrite16(b);
}

uint8_t ILI9341_GFX::readcommand8(uint8_t cmd, uint8_t index) {
  uint8_t idx = 0x10 + index;
  sendCommand(0xD9, &idx, 1);
  _spi->beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  csLow();
  writeCommand(cmd);
  uint8_t r = _spi->transfer(0x00);
  csHigh();
  _spi->endTransaction();
  return r;
}
#endif

// ===========================================================================
// Gemeinsamer Teil
// ===========================================================================

void ILI9341_GFX::sendCommand(uint8_t cmd, const uint8_t *data, uint8_t len) {
  startWrite();
  writeCommand(cmd);
  for (uint8_t i = 0; i < len; i++) spiWrite(data[i]);
  endWrite();
}

FAST_ATTR void ILI9341_GFX::setAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint16_t x1 = x + w - 1, y1 = y + h - 1;
#if ILI9341_GFX_ESP32
  flushBuffer();
  if (directOk()) {
    directWindow(x, y, x1, y1);
    return;
  }
#endif
  if (x != _cx0 || x1 != _cx1) {
    writeAddrCmd(ILI9341_CASET, x, x1);
    _cx0 = x;
    _cx1 = x1;
  }
  if (y != _cy0 || y1 != _cy1) {
    writeAddrCmd(ILI9341_PASET, y, y1);
    _cy0 = y;
    _cy1 = y1;
  }
  writeCommand(ILI9341_RAMWR);
}

FAST_ATTR void ILI9341_GFX::writePixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  fillRectFast(x, y, 1, 1, color);
}

FAST_ATTR void ILI9341_GFX::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  startWrite();
  fillRectFast(x, y, 1, 1, color);
  endWrite();
}

// Rechteck füllen, bereits geclippt: bei leerer Befehlsliste durchgehend
// inline über die Register, sonst über Befehlsliste/DMA
FAST_ATTR void ILI9341_GFX::fillRectFast(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  uint32_t n = (uint32_t)w * h;
#if ILI9341_GFX_ESP32
  if (_bufFill) flushBuffer();
  // Nur noch ein kurzer Rest in der Liste: abwarten und direkt zeichnen. Der
  // Registerweg ist für kleine Flächen schneller als die Liste (ein Interrupt
  // je Eintrag); bei langem Rest bleibt es asynchron.
  if (_busy && n <= 2048 && _pendingBytes <= WAIT_BYTES) waitIdle();
  if (n <= 2048 && directOk()) {
    if (_packed) {
      packedFill(x, y, x + w - 1, y + h - 1, color, n);
      return;
    }
    directWindow(x, y, x + w - 1, y + h - 1);
    if (n == 1) directWrite(true, (color >> 8) | ((color & 0xFF) << 8), 16); // Einzelpixel
    else directFill((color << 8) | (color >> 8), n);
    return;
  }
  if (_packed && (n <= 10 || _pendingBytes > QUEUE_PACK_BYTES)) {
    // Über die Befehlsliste: Fenster und erste Pixel als EIN gepackter
    // Eintrag, der Rest wie gewohnt (DMA-Muster). Strecken mit Rest nur bei
    // langem Rückstau: sonst bliebe die Liste länger beschäftigt und mehr
    // Arbeit liefe über Interrupts statt über den schnelleren Registerweg
    // (gemessen am Testmuster 'D': 84 statt 91 ms).
    uint32_t m = queuePacked(x, y, x + w - 1, y + h - 1, color, n);
    if (n > m) writeColor(color, n - m);
    return;
  }
#endif
  setAddrWindow(x, y, w, h);
  writeColor(color, n);
}

FAST_ATTR void ILI9341_GFX::hline(int16_t x, int16_t y, int16_t w, uint16_t color) {
  if ((uint16_t)y >= (uint16_t)_height) return;
  if (w < 0) {
    x += w + 1;
    w = -w;
  }
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (x + w > _width) w = _width - x;
  if (w <= 0) return;
  fillRectFast(x, y, w, 1, color);
}

FAST_ATTR void ILI9341_GFX::vline(int16_t x, int16_t y, int16_t h, uint16_t color) {
  if ((uint16_t)x >= (uint16_t)_width) return;
  if (h < 0) {
    y += h + 1;
    h = -h;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > _height) h = _height - y;
  if (h <= 0) return;
  fillRectFast(x, y, 1, h, color);
}

FAST_ATTR void ILI9341_GFX::writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w < 0) { x += w + 1; w = -w; }
  if (h < 0) { y += h + 1; h = -h; }
  int32_t x1 = (int32_t)x + w, y1 = (int32_t)y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > _width) x1 = _width;
  if (y1 > _height) y1 = _height;
  if (x >= x1 || y >= y1) return;
  fillRectFast(x, y, x1 - x, y1 - y, color);
}

FAST_ATTR void ILI9341_GFX::writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) { hline(x, y, w, color); }

FAST_ATTR void ILI9341_GFX::writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) { vline(x, y, h, color); }

void ILI9341_GFX::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  startWrite();
  writeFillRect(x, y, w, h, color);
  endWrite();
}

void ILI9341_GFX::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  fillRect(x, y, w, 1, color);
}

void ILI9341_GFX::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  fillRect(x, y, 1, h, color);
}

void ILI9341_GFX::fillScreen(uint16_t color) {
  fillRect(0, 0, _width, _height, color);
}

void ILI9341_GFX::pushImage(int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint16_t *data, bool bigEndian) {
  if (w <= 0 || h <= 0) return;
  int16_t sx = 0, sy = 0, cw = w, ch = h;
  if (x < 0) { sx = -x; cw += x; x = 0; }
  if (y < 0) { sy = -y; ch += y; y = 0; }
  if (x + cw > _width) cw = _width - x;
  if (y + ch > _height) ch = _height - y;
  if (cw <= 0 || ch <= 0) return;

  startWrite();
  setAddrWindow(x, y, cw, ch);
  const uint16_t *src = data + (int32_t)sy * w + sx;
  if (cw == w) {
    writePixels(src, (uint32_t)cw * ch, bigEndian);
  } else {
    for (int16_t row = 0; row < ch; row++, src += w) writePixels(src, cw, bigEndian);
  }
  endWrite();
}

void ILI9341_GFX::drawCanvas(int16_t x, int16_t y, GFXcanvas16 &canvas) {
  // Der Puffer hat immer die Rohmaße des Canvas (unabhängig von dessen Rotation)
  bool swapped = canvas.getRotation() & 1;
  int16_t w = swapped ? canvas.height() : canvas.width();
  int16_t h = swapped ? canvas.width() : canvas.height();
  pushImage(x, y, w, h, canvas.getBuffer());
}

// ---------------------------------------------------------------------------
// Schnelle Zeichenfunktionen (pixelgleich mit Adafruit_GFX)

// Bresenham wie Adafruit_GFX::writeLine, aber zusammenhängende Pixel einer
// Zeile bzw. Spalte gehen als eine Strecke (ein Fenster) raus
FAST_ATTR void ILI9341_GFX::writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  if (y0 == y1) {
    if (x1 < x0) std::swap(x0, x1);
    hline(x0, y0, x1 - x0 + 1, color);
    return;
  }
  if (x0 == x1) {
    if (y1 < y0) std::swap(y0, y1);
    vline(x0, y0, y1 - y0 + 1, color);
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
  int16_t dx = x1 - x0, dy = abs(y1 - y0), err = dx / 2, ystep = y0 < y1 ? 1 : -1;
  int16_t runStart = x0;
  for (; x0 <= x1; x0++) {
    err -= dy;
    if (err < 0 || x0 == x1) {
      int16_t len = x0 - runStart + 1;
      if (steep) vline(y0, runStart, len, color);
      else hline(runStart, y0, len, color);
      if (err < 0) {
        y0 += ystep;
        err += dx;
      }
      runStart = x0 + 1;
    }
  }
}

// Punktfolge von Adafruit_GFX::drawCircleHelper, zu Strecken gruppiert:
// Schritte mit gleichem y ergeben je Achtel eine waagerechte bzw. senkrechte Strecke
FAST_ATTR void ILI9341_GFX::circleRuns(int16_t x0, int16_t y0, int16_t r, uint8_t corners, uint16_t color) {
  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
  int16_t xs = -1, ys = 0;
  // Reihenfolge wie bei LovyanGFX: von einer Strecke zur nächsten ändert
  // sich nur Zeile oder Spalte, das andere Adressregister bleibt gesetzt
  // (spart je Strecke zwei von fünf Übertragungen). Gleiche Pixel wie Adafruit.
  auto flush = [&](int16_t xa, int16_t xb, int16_t yy) {
    int16_t n = xb - xa + 1;
    if (corners & 0x4) hline(x0 + xa, y0 + yy, n, color);
    if (corners & 0x8) hline(x0 - xb, y0 + yy, n, color);
    if (corners & 0x1) hline(x0 - xb, y0 - yy, n, color);
    if (corners & 0x2) hline(x0 + xa, y0 - yy, n, color);
    if (corners & 0x4) vline(x0 + yy, y0 + xa, n, color);
    if (corners & 0x2) vline(x0 + yy, y0 - xb, n, color);
    if (corners & 0x1) vline(x0 - yy, y0 - xb, n, color);
    if (corners & 0x8) vline(x0 - yy, y0 + xa, n, color);
  };
  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }
    x++;
    ddF_x += 2;
    f += ddF_x;
    if (xs < 0) {
      xs = x;
      ys = y;
    } else if (y != ys) {
      flush(xs, x - 1, ys);
      xs = x;
      ys = y;
    }
  }
  if (xs >= 0) flush(xs, x, ys);
}

FAST_ATTR void ILI9341_GFX::drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  startWrite();
  writePixel(x0, y0 + r, color);
  writePixel(x0, y0 - r, color);
  writePixel(x0 + r, y0, color);
  writePixel(x0 - r, y0, color);
  circleRuns(x0, y0, r, 0xF, color);
  endWrite();
}

FAST_ATTR void ILI9341_GFX::drawCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corners, uint16_t color) {
  startWrite();
  circleRuns(x0, y0, r, corners, color);
  endWrite();
}

FAST_ATTR void ILI9341_GFX::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
  int16_t maxRadius = (w < h ? w : h) / 2;
  if (r > maxRadius) r = maxRadius;
  startWrite();
  hline(x + r, y, w - 2 * r, color);
  hline(x + r, y + h - 1, w - 2 * r, color);
  vline(x, y + r, h - 2 * r, color);
  vline(x + w - 1, y + r, h - 2 * r, color);
  circleRuns(x + r, y + r, r, 1, color);
  circleRuns(x + w - r - 1, y + r, r, 2, color);
  circleRuns(x + w - r - 1, y + h - r - 1, r, 4, color);
  circleRuns(x + r, y + h - r - 1, r, 8, color);
  endWrite();
}

// Ein Zeichen: Standardfont mit Hintergrund als ein Fenster mit gestreamten
// Pixeln (wie TFT_eSPI), sonst Strecken je Glyphenzeile
FAST_ATTR void ILI9341_GFX::fastChar(int16_t x, int16_t y, unsigned char ch, uint16_t color, uint16_t bg,
                           uint8_t sx, uint8_t sy) {
  if (!gfxFont) {
    if (x >= _width || y >= _height || x + 6 * sx - 1 < 0 || y + 8 * sy - 1 < 0) return;
    if (!_cp437 && ch >= 176) ch++;
    uint8_t col[5];
    for (int i = 0; i < 5; i++) col[i] = pgm_read_byte(&font[ch * 5 + i]);
    startWrite();
    const int16_t cw = 6 * sx;
    bool inside = x >= 0 && y >= 0 && x + cw <= _width && y + 8 * sy <= _height;
    if (bg != color && inside && cw <= 64) {
      setAddrWindow(x, y, cw, 8 * sy);
      uint16_t line[64];
      for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 6; i++) {
          uint16_t c = (i < 5 && ((col[i] >> j) & 1)) ? color : bg;
          for (int k = 0; k < sx; k++) line[i * sx + k] = c;
        }
        for (int k = 0; k < sy; k++) writePixels(line, cw);
      }
    } else {
      if (bg != color) writeFillRect(x, y, cw, 8 * sy, bg);
      for (int j = 0; j < 8; j++) {
        int i = 0;
        while (i < 5) {
          if (!((col[i] >> j) & 1)) {
            i++;
            continue;
          }
          int s = i;
          while (i < 5 && ((col[i] >> j) & 1)) i++;
          writeFillRect(x + s * sx, y + j * sy, (i - s) * sx, sy, color);
        }
      }
    }
    endWrite();
    return;
  }

  // Eigener Font (immer transparent): Bitmap zeilenweise, Läufe als Rechtecke
  const GFXglyph *glyph = &gfxFont->glyph[ch - (uint8_t)pgm_read_byte(&gfxFont->first)];
  const uint8_t *bitmap = gfxFont->bitmap + pgm_read_word(&glyph->bitmapOffset);
  uint8_t w = pgm_read_byte(&glyph->width), h = pgm_read_byte(&glyph->height);
  int16_t xo = (int8_t)pgm_read_byte(&glyph->xOffset), yo = (int8_t)pgm_read_byte(&glyph->yOffset);
  uint8_t bits = 0, bit = 0;
  startWrite();
  for (uint8_t yy = 0; yy < h; yy++) {
    int16_t run = -1;
    for (uint8_t xx = 0; xx < w; xx++) {
      if (!(bit++ & 7)) bits = pgm_read_byte(bitmap++);
      bool on = bits & 0x80;
      bits <<= 1;
      if (on && run < 0) run = xx;
      if (!on && run >= 0) {
        writeFillRect(x + (xo + run) * sx, y + (yo + yy) * sy, (xx - run) * sx, sy, color);
        run = -1;
      }
    }
    if (run >= 0) writeFillRect(x + (xo + run) * sx, y + (yo + yy) * sy, (w - run) * sx, sy, color);
  }
  endWrite();
}

// Gleiche Cursor-Logik wie Adafruit_GFX::write(), aber mit fastChar()
FAST_ATTR size_t ILI9341_GFX::write(uint8_t c) {
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

uint8_t ILI9341_GFX::rotationMadctl() const {
  switch (rotation) {
  case 0:  return ILI9341_MADCTL_MX | ILI9341_MADCTL_BGR;
  case 1:  return ILI9341_MADCTL_MV | ILI9341_MADCTL_BGR;
  case 2:  return ILI9341_MADCTL_MY | ILI9341_MADCTL_BGR;
  default: return ILI9341_MADCTL_MX | ILI9341_MADCTL_MY | ILI9341_MADCTL_MV | ILI9341_MADCTL_BGR;
  }
}

void ILI9341_GFX::setMadctl(uint8_t value) {
  sendCommand(ILI9341_MADCTL, &value, 1);
  invalidateAddrCache();
}

void ILI9341_GFX::setRotation(uint8_t r) {
  rotation = r & 3;
  uint8_t m = rotationMadctl();
  if (rotation & 1) {
    _width = ILI9341_TFTHEIGHT;
    _height = ILI9341_TFTWIDTH;
  } else {
    _width = ILI9341_TFTWIDTH;
    _height = ILI9341_TFTHEIGHT;
  }
  sendCommand(ILI9341_MADCTL, &m, 1);
  invalidateAddrCache();
}

void ILI9341_GFX::invertDisplay(bool invert) {
  sendCommand(invert ? ILI9341_INVON : ILI9341_INVOFF);
}

void ILI9341_GFX::setScrollMargins(uint16_t top, uint16_t bottom) {
  if (top + bottom > ILI9341_TFTHEIGHT) return;
  _scrTop = top;
  _scrBottom = bottom;
  uint16_t middle = ILI9341_TFTHEIGHT - top - bottom;
  uint8_t d[6] = {(uint8_t)(top >> 8), (uint8_t)top,
                  (uint8_t)(middle >> 8), (uint8_t)middle,
                  (uint8_t)(bottom >> 8), (uint8_t)bottom};
  sendCommand(ILI9341_VSCRDEF, d, 6);
}

void ILI9341_GFX::scrollTo(uint16_t y) {
  _scrVsp = y;
  uint8_t d[2] = {(uint8_t)(y >> 8), (uint8_t)y};
  sendCommand(ILI9341_VSCRSADD, d, 2);
}

void ILI9341_GFX::enableDisplay(bool enable) {
  sendCommand(enable ? ILI9341_DISPON : ILI9341_DISPOFF);
}

void ILI9341_GFX::enableSleep(bool enable) {
  sendCommand(enable ? ILI9341_SLPIN : ILI9341_SLPOUT);
  finishWrites();
  delay(120);
}

void ILI9341_GFX::setIdleMode(bool enable) {
  sendCommand(enable ? ILI9341_IDMON : ILI9341_IDMOFF);
}

void ILI9341_GFX::setPartialMode(bool enable, uint16_t first, uint16_t last) {
  if (!enable) {
    sendCommand(ILI9341_NORON); // beendet laut Datenblatt auch den Scroll-Modus
    setScrollMargins(_scrTop, _scrBottom);
    scrollTo(_scrVsp);
    return;
  }
  uint8_t d[4] = {(uint8_t)(first >> 8), (uint8_t)first, (uint8_t)(last >> 8), (uint8_t)last};
  sendCommand(ILI9341_PTLAR, d, 4);
  sendCommand(ILI9341_PTLON);
}

#if ILI9341_GFX_ESP32
// Prüfmuster für autoSpeed: deterministisch, damit beim Vergleich nichts
// gespeichert werden muss
static inline uint16_t testPixel(uint32_t i, uint32_t seed) {
  uint32_t v = (i + 1) * 2654435761u ^ seed;
  v ^= v >> 15;
  return (uint16_t)(v * 2246822519u >> 16);
}

static inline uint16_t cellColor(int cx, int cy, uint32_t seed) {
  return testPixel(cx * 977 + cy * 131, seed ^ 0xA5A5A5A5);
}

uint32_t ILI9341_GFX::autoSpeed(uint32_t maxFreq) {
  static const uint32_t candidates[] = {80000000, 40000000, 26666667, 20000000, 16000000, 10000000};
  const int16_t W = _width;
  const int16_t BAND = 40; // Zeilen je Testteil
  uint16_t line[ILI9341_TFTHEIGHT];
  uint32_t found = 0;

  for (uint32_t f : candidates) {
    if (f > maxFreq) continue;
    setSPISpeed(f);
    bool ok = true;
    for (uint32_t round = 0; round < 3 && ok; round++) {
      uint32_t seed = 0x9E3779B9u * (round + 1) ^ f;
      // Teil A: ein großes Fenster mit Zufallsdaten (Datenpfad, DMA)
      startWrite();
      setAddrWindow(0, 0, W, BAND);
      for (int32_t i = 0; i < (int32_t)W * BAND;) {
        uint16_t buf[64];
        int n = min(64, (int)((int32_t)W * BAND - i));
        for (int k = 0; k < n; k++) buf[k] = testPixel(i + k, seed);
        writePixels(buf, n);
        i += n;
      }
      // Teil B: viele 8x8-Fenster (Kommandos und Adressen)
      for (int cy = 0; cy < BAND / 8; cy++)
        for (int cx = 0; cx < W / 8; cx++) {
          setAddrWindow(cx * 8, BAND + cy * 8, 8, 8);
          writeColor(cellColor(cx, cy, seed), 64);
        }
      endWrite();
      finishWrites();

      for (int16_t y = 0; y < 2 * BAND && ok; y++) {
        readRect(0, y, W, 1, line);
        for (int16_t x = 0; x < W; x++) {
          uint16_t want = y < BAND ? testPixel((int32_t)y * W + x, seed)
                                   : cellColor(x / 8, (y - BAND) / 8, seed);
          // Readback liefert 6 Bit je Kanal; Rot/Blau-LSB gibt es im 565-Format nicht
          if (line[x] != want) {
            ok = false;
            break;
          }
        }
      }
    }
    if (ok) {
      found = f;
      break;
    }
  }

  // Ein verfälschtes Kommando könnte das Panel verstellt haben: sauber neu starten
  _freq = found ? found : candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];
  _clkWrite = calcClock(_freq);
  hw->clock.val = _clkWrite;
  hwReset();
  runInitSequence(*this);
  setRotation(rotation);
  return found;
}
#endif
