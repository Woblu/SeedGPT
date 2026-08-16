#!/usr/bin/env bash
# Build cubiomes as a static lib, then any program passed as $1.
#   ./build.sh              -> just the library (and src/ objects)
#   ./build.sh find.c       -> library + find.exe
#
# clang here targets x86_64-pc-windows-msvc: no POSIX headers, no -lm.
# cubiomes/compat supplies the sys/time.h it expects.
set -e
cd "$(dirname "$0")"

CUB=cubiomes
OBJ=build/obj
CFLAGS="-O3 -march=native -I$CUB -I$CUB/compat -Isrc -I$CUB/loot/cjson -Wno-parentheses -Wno-unused-function"
JOBS=$(nproc 2>/dev/null || echo 4)

mkdir -p "$OBJ"

# Everything except the entry points cubiomes ships (tests.c, xradv.c).
SRCS=$(find "$CUB" -name '*.c' -not -path '*/.git/*' \
       -not -name 'tests.c' -not -name 'xradv.c' | sort)

# One object, reporting failure rather than swallowing it. clang writes warnings
# to the same stream, so the grep selects only real errors -- and its exit code
# must NOT become the compile's exit code, which is what the old
# `&& exit 1 || true` was juggling.
compile_one() {
  local src="$1" obj="$2" log
  log=$(clang $CFLAGS -c "$src" -o "$obj" 2>&1) || { printf '%s\n' "$log"; return 1; }
  if printf '%s\n' "$log" | grep -qE "error|fatal"; then
    printf '%s\n' "$log"
    return 1
  fi
  return 0
}
export -f compile_one
export CFLAGS

# Compile a whitespace-separated file list in parallel. xargs exits non-zero if
# any child does, and `set -e` turns that into a failed build.
compile_all() {
  local pairs=""
  local s o
  for s in $1; do
    o=$(eval "echo $2")
    pairs+="$s"$'\n'"$o"$'\n'
  done
  printf '%s' "$pairs" | xargs -d '\n' -n2 -P "$JOBS" \
    bash -c 'compile_one "$0" "$1"'
}

STAMP="$OBJ/.built"
# Watch our own headers too: a change to src/*.h that alters how cubiomes
# headers are seen (an include, a macro) must force a recompile, or a stale
# object links against a mismatched declaration and the binary crashes.
NEWEST=$(ls -t $SRCS "$CUB"/*.h src/*.h 2>/dev/null | head -1)
if [ ! -f "$STAMP" ] || [ "$NEWEST" -nt "$STAMP" ]; then
  echo "compiling cubiomes ($(echo "$SRCS" | wc -l) files, $JOBS at a time)..."
  compile_all "$SRCS" '"$OBJ/$(echo "${s#$CUB/}" | tr "/" "_" | sed "s/\.c$/.o/")"'
  touch "$STAMP"
else
  echo "cubiomes objects up to date"
fi

# src/*.c used to be recompiled from source on EVERY link, so building 27 tools
# compiled these ten files 270 times. They are objects now, cached on the same
# terms as cubiomes. This is the single biggest cost in ./test.sh.
SRC_C=$(ls src/*.c 2>/dev/null | sort)
SSTAMP="$OBJ/.built_src"
NEWEST_SRC=$(ls -t $SRC_C src/*.h "$CUB"/*.h 2>/dev/null | head -1)
if [ -n "$SRC_C" ] && { [ ! -f "$SSTAMP" ] || [ "$NEWEST_SRC" -nt "$SSTAMP" ]; }; then
  echo "compiling src ($(echo "$SRC_C" | wc -w) files)..."
  compile_all "$SRC_C" '"$OBJ/src_$(basename "${s%.c}").o"'
  touch "$SSTAMP"
fi

if [ -n "${1:-}" ]; then
  out="build/$(basename "${1%.c}").exe"
  echo "linking $out"
  clang $CFLAGS "$1" "$OBJ"/*.o -o "$out"
  echo "built $out"
fi
