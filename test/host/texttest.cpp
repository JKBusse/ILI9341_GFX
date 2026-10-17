#include "ILI9341_GFX.h"
#include "ILI9341_Framebuffer.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <stdio.h>
int g_pin[64]; SPIClass SPI;
void pinMode(int,int){} void digitalWrite(int p,int v){g_pin[p]=v;} void delay(unsigned long){}
void SPIClass::write(uint8_t){}
#ifndef FB_TYPE
#define FB_TYPE ILI9341_Framebuffer
#endif
ILI9341_GFX tft(15,2,-1); FB_TYPE a(tft), b(tft);
struct Case { const GFXfont* f; uint8_t sx, sy; uint16_t fg, bg; int16_t x, y; bool wrap; };
int main(){ tft.begin(); if(!a.begin()||!b.begin()) return 1; int fails=0, cases=0;
  const GFXfont* fonts[]={nullptr,&FreeSans9pt7b,&FreeSansBold9pt7b,&FreeSansBold18pt7b};
  const char* txt="Hallo Welt! 0123456789 ABCxyz {}|~ \xb0\xe4\xf6\xfc\xb5 \nZweite Zeile_mit:Text;";
  for(int rot=0;rot<4;rot++){ a.setRotation(rot); b.setRotation(rot);
   for(auto f: fonts) for(int sz=1;sz<=3;sz++) for(int mode=0;mode<3;mode++) for(int pos=0;pos<4;pos++){
    uint16_t fg=0xF81F, bg = mode==0? fg : (mode==1? 0x001F : 0x07E0);
    int16_t x = pos==0? 3 : pos==1? -7 : pos==2? a.width()-40 : 20, y = pos==3? -5 : (pos==2? a.height()-10 : 30);
    for(FB_TYPE* fb : {&a,&b}){ fb->fillScreen(0x1234); fb->setFont(f); fb->setTextSize(sz, sz+(mode==2)); fb->setTextColor(fg,bg);
      fb->setTextWrap(pos!=1); fb->setCursor(x,y); fb->cp437(mode==1); }
    for(const char*p=txt;*p;p++){ a.write((uint8_t)*p); b.Adafruit_GFX::write((uint8_t)*p); }
    int bad=0; for(int yy=0;yy<a.height();yy++) for(int xx=0;xx<a.width();xx++) bad += a.getPixel(xx,yy)!=b.getPixel(xx,yy);
    if(a.getCursorX()!=b.getCursorX()||a.getCursorY()!=b.getCursorY()) bad++;
    cases++; if(bad){ fails++; if(fails<5) printf("FEHLER rot%d font%p size%d mode%d pos%d: %d px\n",rot,(void*)f,sz,mode,pos,bad);} }
   // Linien: schnelle Version vs. Adafruit-Bresenham
   srand(rot); for(FB_TYPE* fb : {&a,&b}) fb->fillScreen(0);
   for(int i=0;i<3000;i++){ int R = i<1000 ? 400 : (i<2000 ? 3000 : 30); int x0=rand()%R-R/4,y0=rand()%R-R/4,x1=rand()%R-R/4,y1=rand()%R-R/4;
     if(i%7==0){ x1=x0+rand()%3-1; } if(i%11==0){ y1=y0; } uint16_t c=rand();
     a.drawLine(x0,y0,x1,y1,c); b.Adafruit_GFX::writeLine(x0,y0,x1,y1,c); }
   int bad=0; for(int yy=0;yy<a.height();yy++) for(int xx=0;xx<a.width();xx++) bad += a.getPixel(xx,yy)!=b.getPixel(xx,yy);
   cases++; if(bad){fails++; printf("FEHLER Linien rot%d: %d px\n",rot,bad);}
   // Dreiecke und Kreise: schnelle Version vs. Adafruit
   for(FB_TYPE* fb : {&a,&b}) fb->fillScreen(0);
   for(int i=0;i<400;i++){ int16_t p[6]; for(int k=0;k<6;k++) p[k]=rand()%440-60; uint16_t c=rand();
     a.fillTriangle(p[0],p[1],p[2],p[3],p[4],p[5],c); b.Adafruit_GFX::fillTriangle(p[0],p[1],p[2],p[3],p[4],p[5],c); }
   bad=0; for(int yy=0;yy<a.height();yy++) for(int xx=0;xx<a.width();xx++) bad += a.getPixel(xx,yy)!=b.getPixel(xx,yy);
   cases++; if(bad){fails++; printf("FEHLER Dreiecke rot%d: %d px\n",rot,bad);}
   for(FB_TYPE* fb : {&a,&b}) fb->fillScreen(0);
   for(int i=0;i<300;i++){ int16_t x=rand()%440-60, y=rand()%440-60, r = i<20 ? i : rand()%200; uint16_t c=rand();
     a.fillCircle(x,y,r,c); b.Adafruit_GFX::fillCircle(x,y,r,c); }
   bad=0; for(int yy=0;yy<a.height();yy++) for(int xx=0;xx<a.width();xx++) bad += a.getPixel(xx,yy)!=b.getPixel(xx,yy);
   cases++; if(bad){fails++; printf("FEHLER Kreise rot%d: %d px\n",rot,bad);} }
  printf("%d Fälle, %s\n", cases, fails? "FEHLGESCHLAGEN":"alle pixelgleich mit Adafruit_GFX"); return fails!=0; }
