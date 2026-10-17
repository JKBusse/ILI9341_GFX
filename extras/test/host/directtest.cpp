#include "ILI9341_GFX.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <stdio.h>
#include <vector>
int g_pin[64]; SPIClass SPI;
void pinMode(int,int){} void digitalWrite(int p,int v){g_pin[p]=v;} void delay(unsigned long){}
const int DC=2, CS=15;
static uint16_t sim[320][320]; static uint8_t cmd, prm[4]; static int np, cx0,cx1,cy0,cy1,px,py; static uint8_t hi; static bool haveHi;
void SPIClass::write(uint8_t b){
  if(!g_pin[DC]){cmd=b;np=0;haveHi=false; if(cmd==0x2C){px=cx0;py=cy0;} return;}
  if(cmd==0x2A||cmd==0x2B){prm[np++]=b; if(np==4){int a=prm[0]<<8|prm[1], e=prm[2]<<8|prm[3]; if(cmd==0x2A){cx0=a;cx1=e;}else{cy0=a;cy1=e;}}}
  else if(cmd==0x2C){ if(!haveHi){hi=b;haveHi=true;return;} haveHi=false; if(py<=cy1 && py<320 && px<320) sim[py][px]=hi<<8|b; if(++px>cx1){px=cx0;py++;} } }
ILI9341_GFX tft(CS,DC,-1);
std::vector<uint16_t> snap(){ return std::vector<uint16_t>(&sim[0][0], &sim[0][0]+320*320); }
template<class F> int compare(const char* name, F draw){
  tft.fillScreen(0x0841); draw(true); auto a=snap();
  tft.fillScreen(0x0841); draw(false); auto b=snap();
  int bad=0; for(size_t i=0;i<a.size();i++) bad += a[i]!=b[i];
  if(bad) printf("FEHLER %s: %d px\n", name, bad); return bad!=0; }
int main(){ int fails=0, cases=0; tft.begin();
  const GFXfont* fonts[]={nullptr,&FreeSans9pt7b,&FreeSansBold18pt7b};
  for(int rot=0;rot<4;rot++){ tft.setRotation(rot);
    cases++; fails+=compare("Linien",[&](bool fast){ srand(rot); for(int i=0;i<400;i++){ int R=i<300?400:3000; int x0=rand()%R-R/4,y0=rand()%R-R/4,x1=rand()%R-R/4,y1=rand()%R-R/4; uint16_t c=rand();
        if(fast) tft.drawLine(x0,y0,x1,y1,c); else tft.Adafruit_GFX::writeLine(x0,y0,x1,y1,c);} });
    cases++; fails+=compare("Kreise",[&](bool fast){ srand(rot+10); for(int i=0;i<200;i++){ int x=rand()%440-60,y=rand()%440-60,r=i<25?i:rand()%180; uint16_t c=rand();
        if(fast) tft.drawCircle(x,y,r,c); else tft.Adafruit_GFX::drawCircle(x,y,r,c);} });
    cases++; fails+=compare("RoundRects",[&](bool fast){ srand(rot+20); for(int i=0;i<200;i++){ int x=rand()%400-40,y=rand()%400-40,w=rand()%200,h=rand()%200,r=rand()%60; uint16_t c=rand();
        if(fast) tft.drawRoundRect(x,y,w,h,r,c); else tft.Adafruit_GFX::drawRoundRect(x,y,w,h,r,c);} });
    cases++; fails+=compare("FillRoundRects",[&](bool fast){ srand(rot+30); for(int i=0;i<200;i++){ int x=rand()%400-40,y=rand()%400-40,w=rand()%200,h=rand()%200,r=rand()%60; uint16_t c=rand();
        if(fast) tft.fillRoundRect(x,y,w,h,r,c); else tft.Adafruit_GFX::fillRoundRect(x,y,w,h,r,c);} });
    const char* txt="Hallo Welt! 0123 ABCxyz {}|~ \xb0\xe4\xf6\xfc\nZweite Zeile_mit:Text;";
    for(auto f: fonts) for(int sz=1;sz<=4;sz++) for(int mode=0;mode<3;mode++) for(int pos=0;pos<4;pos++){
      cases++; char nm[64]; snprintf(nm,64,"Text r%d f%p s%d m%d p%d",rot,(void*)f,sz,mode,pos);
      fails+=compare(nm,[&](bool fast){ uint16_t fg=0xF81F, bg= mode==0?fg:(mode==1?0x001F:0x07E0);
        int16_t x = pos==0?3: pos==1?-7: pos==2? tft.width()-40 : 20, y = pos==3?-5:(pos==2? tft.height()-10:30);
        tft.setFont(f); tft.setTextSize(sz, sz+(mode==2)); tft.setTextColor(fg,bg); tft.setTextWrap(pos!=1); tft.setCursor(x,y); tft.cp437(mode==1);
        for(const char*p=txt;*p;p++){ if(fast) tft.write((uint8_t)*p); else tft.Adafruit_GFX::write((uint8_t)*p);} }); }
  }
  printf("%d Fälle, %s\n", cases, fails?"FEHLGESCHLAGEN":"alle pixelgleich mit Adafruit_GFX"); return fails!=0; }
