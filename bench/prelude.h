// Prototypen der Hooks, die bench.h braucht (Definition in der jeweiligen Datei)
#pragma once
#include <Arduino.h>
extern const char *LIB_NAME;
void benchSync();
void benchPushStrip(int16_t y, uint16_t *buf);
bool animBegin(String &info);
void animDraw(int frame, bool full);
void animPush();
