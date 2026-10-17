#include "ILI9341_GFX.h"
#include "ILI9341_Framebuffer.h"
#include <stdio.h>
int g_pin[64]; SPIClass SPI;
void pinMode(int,int){} void digitalWrite(int p,int v){g_pin[p]=v;} void delay(unsigned long){}
const int DC=2, CS=15;
// --- simulierter ILI9341 (logische Koordinaten) ---
static uint16_t sim[320][320]; static uint8_t cmd; static uint8_t prm[4]; static int np;
static int cx0,cx1,cy0,cy1,px,py; static uint8_t hi; static bool haveHi; static long bytes;
static int tfa=0,vsa=320,bfa=0,vsp=0; static uint8_t sp[6];
void SPIClass::write(uint8_t b){ bytes++;
  if(g_pin[CS]) { printf("FEHLER: Byte ohne CS\n"); exit(1);} 
  if(!g_pin[DC]){cmd=b;np=0;haveHi=false; if(cmd==0x2C){px=cx0;py=cy0;} return;}
  if(cmd==0x33){sp[np++]=b; if(np==6){tfa=sp[0]<<8|sp[1]; vsa=sp[2]<<8|sp[3]; bfa=sp[4]<<8|sp[5];} return;}
  if(cmd==0x37){sp[np++]=b; if(np==2){vsp=sp[0]<<8|sp[1];} return;}
  if(cmd==0x2A||cmd==0x2B){prm[np++]=b; if(np==4){int a=prm[0]<<8|prm[1], e=prm[2]<<8|prm[3]; if(cmd==0x2A){cx0=a;cx1=e;}else{cy0=a;cy1=e;}}}
  else if(cmd==0x2C){ if(!haveHi){hi=b;haveHi=true;return;} haveHi=false;
    if(py<=cy1) sim[py][px]=hi<<8|b; if(++px>cx1){px=cx0;py++;} }
}
ILI9341_GFX tft(CS,DC,-1);
#ifndef FB_TYPE
#define FB_TYPE ILI9341_Framebuffer
#endif
FB_TYPE fb(tft);
// unabhängige Nachbildung: angezeigte Gate-Leitung -> Speicherzeile
static int memOf(int g){ if(g<tfa||g>=tfa+vsa) return g; return tfa+((g-tfa+vsp-tfa)%vsa+vsa)%vsa; }
static uint16_t shown(int x,int y){ int r=fb.getRotation();
  int c = (r&1)? x : y;                 // Koordinate entlang der Scrollachse
  int g = r>=2 ? 319-c : c; int m = memOf(g); int cm = r>=2 ? 319-m : m;
  return (r&1) ? sim[y][cm] : sim[cm][x]; }
int check(const char*name){ int bad=0; for(int y=0;y<fb.height();y++)for(int x=0;x<fb.width();x++) if(shown(x,y)!=fb.getPixel(x,y)) bad++;
  printf("%-28s %s  gesendet=%6u px  bytes=%ld\n",name,bad?"FEHLER":"ok",fb.lastPixelsSent(),bytes); bytes=0; return bad; }
int main(){ int fails=0; g_pin[CS]=1;
  tft.begin(); if(!fb.begin()){puts("alloc fail");return 1;}
  for(int rot=0;rot<4;rot++){ fb.setRotation(rot); memset(sim,0xAB,sizeof sim);
    printf("--- Rotation %d (%dx%d)\n",rot,fb.width(),fb.height());
    fb.fillScreen(0x1234); fb.display(); fails+=check("erster Frame (alles)");
    fb.fillScreen(0x1234); fb.display(); fails+=check("identischer Frame");
    fb.fillCircle(100,100,20,0xF800); fb.display(); fails+=check("Kreis");
    fb.fillScreen(0x1234); fb.fillCircle(105,100,20,0xF800); fb.drawPixel(fb.width()-1,fb.height()-1,0xFFFF);
    fb.setCursor(3,3); fb.setTextColor(0x07E0); fb.setTextSize(2); fb.print("Hallo 123"); fb.display(); fails+=check("Redraw + Text + Ecke");
    for(int i=0;i<200;i++){ srand(i*7+rot); fb.fillRect(rand()%400-40,rand()%400-40,rand()%120-30,rand()%120-30,rand()); fb.drawLine(rand()%340-10,rand()%340-10,rand()%340-10,rand()%340-10,rand()); }
    fb.display(); fails+=check("200 Zufallsformen");
    fb.fillScreen(0); fb.display(); fails+=check("clear");
    for(int f=0;f<30;f++){ srand(1000+f*13+rot); int n=rand()%40;
      for(int i=0;i<n;i++) fb.drawPixel(rand()%fb.width(),rand()%fb.height(),rand()|1);
      if(f%3==0) fb.fillRect(rand()%fb.width(),rand()%fb.height(),rand()%60,rand()%8,rand());
      if(f%5==0) fb.drawFastHLine(0,rand()%fb.height(),fb.width(),rand());
      fb.display(); if(check("verstreute Pixel")) {fails++; break;} }
    for(int y=0;y<fb.height();y+=7) fb.drawFastHLine(y%3? 0: fb.width()/2, y, fb.width()/2, 0xF0F0);
    fb.display(); fails+=check("Luecken + halbe Zeilen");
  }
  { // getRow-Test: direkter Zugriff und markDirty
    fb.setRotation(1); fb.fillScreen(0); fb.display();
    auto *r = fb.getRow(100); for(int x=0;x<fb.width();x++) r[x]=(uint16_t)(x*37);
    fb.display(); fails+=check("getRow");
    uint16_t beef=0xBEEF; r[5]=beef; fb.markDirty(5,100,1,1); fb.display(); fails+=check("markDirty");
    fb.drawLine(-20,-30,400,300,0xF00F); fb.drawLine(10,200,300,5,0x0FF0); fb.display(); fails+=check("Linien");
    fb.setFont(); fb.setTextSize(2); fb.setTextColor(0xFFFF,0x0010); fb.setCursor(300,230); fb.print("Rand"); fb.display(); fails+=check("Text am Rand"); }
  // Hardware-Scrolling: alle Rotationen, feste Bereiche, zufällige Schritte
  for(int rot=0;rot<4;rot++){ fb.setRotation(rot); fb.fillScreen(0); fb.display();
    const int areas[4][2]={{0,0},{40,0},{0,24},{17,31}};
    for(int a=0;a<4;a++){ fb.setScrollArea(areas[a][0],areas[a][1]); int bad=0;
      for(int i=0;i<60;i++){ srand(rot*1000+a*100+i);
        int d=(rand()%41)-20; fb.scroll(d);
        int len = 320-areas[a][0]-areas[a][1];
        // frei gewordenen Streifen neu zeichnen + etwas Zufall
        if(fb.getRotation()&1) fb.fillRect(320-areas[a][1]-abs(d),0,abs(d),240,rand());
        else fb.fillRect(0,320-areas[a][1]-abs(d),240,abs(d),rand());
        fb.fillCircle(rand()%fb.width(),rand()%fb.height(),rand()%30,rand());
        fb.drawPixel(rand()%fb.width(),rand()%fb.height(),rand());
        fb.display(); (void)len;
        int b=0; for(int y=0;y<fb.height();y++)for(int x=0;x<fb.width();x++) b+= shown(x,y)!=fb.getPixel(x,y);
        if(b){ bad++; if(bad==1) printf("  Fehler rot%d area%d schritt%d delta=%d: %d px\n",rot,a,i,d,b);} }
      printf("Scroll rot%d fest=(%d,%d)          %s\n",rot,areas[a][0],areas[a][1],bad?"FEHLER":"ok"); fails+=bad; }
  }
#ifdef FB8
  { uint16_t pal[256]; for(int i=0;i<256;i++) pal[i]=(uint16_t)(i*257+13);
    fb.setRotation(1); fb.setPalette(pal);
    for(int y=0;y<fb.height();y++) for(int x=0;x<fb.width();x++) fb.drawPixel(x,y,(x+y)&255);
    fb.display(); fails+=check("Palette");
    for(int i=0;i<256;i++) pal[i]=(uint16_t)(pal[(i+1)&255]^0x5A5A);
    fb.setPalette(pal); fb.display(); fails+=check("Palettenwechsel ohne Neuzeichnen");
    fb.useRGB332(); fb.fillScreen(0xF81F); fb.display(); fails+=check("zurueck zu RGB332"); }
#endif
  printf(fails?"\nFEHLGESCHLAGEN\n":"\nALLE TESTS OK\n"); return fails!=0; }
