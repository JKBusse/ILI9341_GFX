#pragma once
#include "Arduino.h"
struct SPISettings { SPISettings(uint32_t=0,int=0,int=0){} };
class SPIClass { public:
  void begin(){} void beginTransaction(SPISettings){} void endTransaction(){}
  void write(uint8_t b); uint8_t transfer(uint8_t b){write(b);return 0;}
  uint16_t transfer16(uint16_t w){write(w>>8);write(w);return 0;}
};
extern SPIClass SPI;
