#!/bin/sh
# Host-Tests: übersetzen Treiber und Framebuffer für den PC (Nicht-ESP32-Pfad,
# SPI über einen simulierten ILI9341) und vergleichen jedes Pixel mit
# Adafruit_GFX bzw. einer unabhängigen Nachbildung.
#   fbtest      Framebuffer (16 und 8 Bit): Zeichenfunktionen, Dirty-Rects, Scrollen
#   texttest    Text im Framebuffer, alle Fonts/Größen/Rotationen
#   directtest  Direktes Zeichnen (Linien, Kreise, Rundrechtecke, Text)
# Voraussetzung: einmal "pio run -e nodemcu-32s" (lädt Adafruit GFX nach .pio/libdeps)
# oder ADAFRUIT_GFX=<Pfad zur Adafruit GFX Library>.
set -e
cd "$(dirname "$0")"
ROOT=../../..
LIB=$ROOT/src
GFX="${ADAFRUIT_GFX:-$ROOT/.pio/libdeps/nodemcu-32s/Adafruit GFX Library}" # in der CI: eigener Checkout
[ -d "$GFX" ] || { echo "Adafruit GFX fehlt: erst 'pio run -e nodemcu-32s' ausführen oder ADAFRUIT_GFX setzen"; exit 1; }
CXX=${CXX:-c++}
mkdir -p build
build() { # name quelle [defines]
  out=$1; src=$2; shift 2
  $CXX -std=c++17 -O1 -DARDUINO=100 "$@" -Istubs -I"$LIB" -I"$GFX" "$src" \
    "$LIB/ILI9341_GFX.cpp" "$LIB/ILI9341_Framebuffer.cpp" "$GFX/Adafruit_GFX.cpp" -o "build/$out"
}
build fbtest fbtest.cpp
build fbtest8 fbtest.cpp -DFB_TYPE=ILI9341_Framebuffer8
build texttest texttest.cpp
build directtest directtest.cpp
fail=0
for t in fbtest fbtest8 texttest directtest; do
  if ./build/$t > build/$t.log; then echo "ok      $t: $(tail -1 build/$t.log)"
  else echo "FEHLER  $t (Details: extras/test/host/build/$t.log)"; fail=1; fi
done
exit $fail
