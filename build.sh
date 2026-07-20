#!/usr/bin/env bash
# Build cubiomes as a static lib, then any program passed as $1.
#   ./build.sh              -> just the library
#   ./build.sh find.c       -> library + find.exe
#
# clang here targets x86_64-pc-windows-msvc: no POSIX headers, no -lm.
# cubiomes/compat supplies the sys/time.h it expects.
set -e
cd "$(dirname "$0")"

CUB=cubiomes
OBJ=build/obj
CFLAGS="-O3 -march=native -I$CUB -I$CUB/compat -Isrc -I$CUB/loot/cjson -Wno-parentheses -Wno-unused-function"

mkdir -p "$OBJ"

# Everything except the entry points cubiomes ships (tests.c, xradv.c).
SRCS=$(find "$CUB" -name '*.c' -not -path '*/.git/*' \
       -not -name 'tests.c' -not -name 'xradv.c' | sort)

STAMP="$OBJ/.built"
NEWEST=$(ls -t $SRCS "$CUB"/*.h 2>/dev/null | head -1)
if [ ! -f "$STAMP" ] || [ "$NEWEST" -nt "$STAMP" ]; then
  echo "compiling cubiomes ($(echo "$SRCS" | wc -l) files)..."
  for s in $SRCS; do
    o="$OBJ/$(echo "${s#$CUB/}" | tr '/' '_' | sed 's/\.c$/.o/')"
    clang $CFLAGS -c "$s" -o "$o" 2>&1 | grep -E "error|fatal" && exit 1 || true
  done
  touch "$STAMP"
else
  echo "cubiomes objects up to date"
fi

if [ -n "$1" ]; then
  out="build/$(basename "${1%.c}").exe"
  echo "linking $out"
  clang $CFLAGS "$1" src/*.c "$OBJ"/*.o -o "$out"
  echo "built $out"
fi
