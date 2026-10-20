#!/usr/bin/env bash
# Build cubiomes as a static lib, then any program passed as $1.
#   ./build.sh              -> just the library (and src/ objects)
#   ./build.sh find.c       -> library + find.exe
#   ./build.sh all          -> library + every tool in tools/, which is what
#                              serve.py, test.sh and the tier scripts expect
#
# Binaries are named *.exe everywhere so every script finds them by one name.
# On Windows clang targets x86_64-pc-windows-msvc: no POSIX headers, no -lm, and
# cubiomes/compat supplies the sys/time.h it expects. Elsewhere libm and
# pthreads are linked explicitly.
set -e
cd "$(dirname "$0")"

CUB=cubiomes
OBJ=build/obj

case "$(uname -s)" in
  # The MSVC CRT marks fopen/getenv/strcpy "deprecated" in favour of its own
  # _s variants; that is not portable C and not a finding, so -Wall would
  # bury real warnings under it.
  MINGW*|MSYS*|CYGWIN*) LIBS=""; PLATFLAGS="-D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE" ;;
  *)                    LIBS="-lm -pthread"; PLATFLAGS="" ;;
esac

# -ffp-contract=off: Java never fuses a*b+c into a single rounding. clang does by
# default once -march=native exposes FMA -- 393 fused multiply-adds turned up in
# the noise objects. Noise feeds every biome and terrain threshold, so a one-ulp
# difference at a boundary flips a biome or a block that Minecraft would not.
# Matching Java's arithmetic is the whole point of the engine.
#
# MARCH overrides -march=native, e.g. MARCH=x86-64-v2 for a binary that has to
# run on a different machine.
MARCH=${MARCH:-native}
BASEFLAGS="-O3 -march=$MARCH -ffp-contract=off $PLATFLAGS -I$CUB -I$CUB/compat -Isrc -I$CUB/loot/cjson"
# Upstream's code is not ours to lint, so it builds quietly; ours gets -Wall,
# and its warnings are printed rather than swallowed.
CUBFLAGS="$BASEFLAGS -Wno-parentheses -Wno-unused-function"
OURFLAGS="$BASEFLAGS -Wall -Wno-parentheses -Wno-unused-function"

JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

mkdir -p "$OBJ"

# Objects built with different flags are not interchangeable, and nothing else
# here would notice a flag change -- the timestamps say "up to date". Record the
# flags; if they moved, start clean.
FLAGSFILE="$OBJ/.flags"
if [ ! -f "$FLAGSFILE" ] || [ "$(cat "$FLAGSFILE")" != "$CUBFLAGS|$OURFLAGS" ]; then
  [ -f "$FLAGSFILE" ] && echo "compiler flags changed -- rebuilding all objects"
  rm -f "$OBJ"/*.o "$OBJ"/.built "$OBJ"/.built_src
  printf '%s' "$CUBFLAGS|$OURFLAGS" > "$FLAGSFILE"
fi

# Everything except the entry points cubiomes ships (tests.c, xradv.c).
SRCS=$(find "$CUB" -name '*.c' -not -path '*/.git/*' \
       -not -name 'tests.c' -not -name 'xradv.c' | sort)

# One object. clang's exit code is the verdict -- grepping its output for
# "error" also matched warnings quoting a line that happens to contain the word.
# Warnings are shown when the flags ask for them (our code), dropped otherwise.
compile_one() {
  local flags="$1" src="$2" obj="$3" log
  log=$(clang $flags -c "$src" -o "$obj" 2>&1) || { printf '%s\n' "$log" >&2; return 1; }
  if [ -n "$log" ] && [ "$flags" = "$OURFLAGS" ]; then printf '%s\n' "$log" >&2; fi
  return 0
}
export -f compile_one
export CUBFLAGS OURFLAGS

# Compile a whitespace-separated file list in parallel. NUL-separated so xargs
# behaves the same under GNU and BSD; xargs exits non-zero if any child does,
# and `set -e` turns that into a failed build.
compile_all() {
  local flags="$1" files="$2" namer="$3" s o
  for s in $files; do
    o=$(eval "echo $namer")
    printf '%s\0%s\0%s\0' "$flags" "$s" "$o"
  done | xargs -0 -n3 -P "$JOBS" bash -c 'compile_one "$0" "$1" "$2"'
}

STAMP="$OBJ/.built"
# Watch our own headers too: a change to src/*.h that alters how cubiomes
# headers are seen (an include, a macro) must force a recompile, or a stale
# object links against a mismatched declaration and the binary crashes.
NEWEST=$(ls -t $SRCS "$CUB"/*.h src/*.h 2>/dev/null | head -1)
if [ ! -f "$STAMP" ] || [ "$NEWEST" -nt "$STAMP" ]; then
  echo "compiling cubiomes ($(echo "$SRCS" | wc -l) files, $JOBS at a time)..."
  compile_all "$CUBFLAGS" "$SRCS" '"$OBJ/$(echo "${s#$CUB/}" | tr "/" "_" | sed "s/\.c$/.o/")"'
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
  compile_all "$OURFLAGS" "$SRC_C" '"$OBJ/src_$(basename "${s%.c}").o"'
  touch "$SSTAMP"
fi

# An object whose source was deleted or renamed would otherwise be linked into
# every binary through the *.o glob below, forever.
for o in "$OBJ"/src_*.o; do
  [ -e "$o" ] || continue
  n=$(basename "$o" .o); n=${n#src_}
  [ -f "src/$n.c" ] || { echo "removing stale object $o"; rm -f "$o"; }
done

link_one() {
  local src="$1" out
  out="build/$(basename "${src%.c}").exe"
  clang $OURFLAGS "$src" "$OBJ"/*.o -o "$out" $LIBS || return 1
  echo "built $out"
}

if [ "${1:-}" = "all" ]; then
  # Every tool, in parallel; one failure does not hide the rest -- they are
  # listed together at the end and the exit code says the build failed.
  fail=$(mktemp)
  export -f link_one; export OBJ LIBS
  ls tools/*.c | xargs -P "$JOBS" -I{} bash -c 'link_one "{}" || echo "{}" >>"'"$fail"'"'
  if [ -s "$fail" ]; then
    echo "FAILED:" $(cat "$fail") >&2
    rm -f "$fail"
    exit 1
  fi
  rm -f "$fail"
  echo "all tools built"
elif [ -n "${1:-}" ]; then
  echo "linking build/$(basename "${1%.c}").exe"
  link_one "$1"
fi
