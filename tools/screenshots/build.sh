#!/usr/bin/env bash
# Builds the UI for the host and renders the README screenshots to docs/images/.
#
# Requirements: gcc, g++ and git (Linux, macOS or WSL).
# Set U8G2_DIR to an existing U8g2 Arduino library to skip the download, e.g.
#   U8G2_DIR=~/Arduino/libraries/U8g2 tools/screenshots/build.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$HERE/build"
U8G2_VERSION="2.36.19"
U8G2_DIR="${U8G2_DIR:-$BUILD/U8g2_Arduino}"

if [ ! -d "$U8G2_DIR/src/clib" ]; then
  echo "Fetching U8g2 $U8G2_VERSION..."
  git clone --quiet --depth 1 --branch "$U8G2_VERSION" \
    https://github.com/olikraus/U8g2_Arduino.git "$U8G2_DIR"
fi

# U8g2 C sources rarely change: compile them once and reuse the objects
OBJ="$BUILD/obj"
mkdir -p "$OBJ"
export OBJ
find "$U8G2_DIR/src/clib" -name '*.c' -print0 |
  xargs -0 -P "$(nproc 2>/dev/null || echo 4)" -I{} sh -c \
    'o="$OBJ/$(basename "{}" .c).o"; [ "$o" -nt "{}" ] || gcc -O1 -w -c "{}" -o "$o"'

g++ -std=c++17 -O1 -Wall -Wextra \
  -I "$HERE/stubs" -I "$U8G2_DIR/src" -I "$ROOT/src" \
  "$HERE/main.cpp" "$ROOT/src/ui.cpp" "$ROOT/src/calendar.cpp" "$OBJ"/*.o \
  -o "$BUILD/screenshots"

mkdir -p "$ROOT/docs/images"
"$BUILD/screenshots" "$ROOT/docs/images"
