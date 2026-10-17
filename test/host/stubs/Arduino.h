#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include "Print.h"
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define pgm_read_pointer(addr) ((void *)pgm_read_dword(addr)) // wie in Adafruit_GFX.cpp
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define MSBFIRST 1
#define SPI_MODE0 0
typedef bool boolean;
class String { std::string s; public: String(const char*c=""):s(c){} unsigned length() const {return s.size();} const char* c_str() const {return s.c_str();} };
class __FlashStringHelper;
void pinMode(int, int);
void digitalWrite(int, int);
void delay(unsigned long);
extern int g_pin[64];
#define radians(d) ((d)*M_PI/180.0)
#include <algorithm>
using std::min; using std::max;
#define constrain(v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
