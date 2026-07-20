#!/usr/bin/env bash
# Restore the engine. cubiomes/ is gitignored (it's an upstream clone, not our
# source), so a fresh checkout needs this before ./build.sh will work.
#
# The commit is PINNED deliberately. Engine currency is the whole reason this
# project uses xpple/cubiomes rather than Cubitect/cubiomes (dormant since
# 2024-11-10, caps at MC 1.21 — it cannot generate current worlds). Pinning
# means a future upstream change can't silently alter search results; bump it
# on purpose, then re-run ./test.sh.
set -e
cd "$(dirname "$0")"

REPO=https://github.com/xpple/cubiomes.git
PIN=b12a5325c8a195fa99711ed2052a4fc9cac077a8   # 2026-06-29, MC_NEWEST = MC_26_2

if [ -d cubiomes/.git ]; then
  echo "cubiomes/ already present at $(git -C cubiomes rev-parse --short HEAD)"
else
  echo "cloning $REPO"
  git clone "$REPO" cubiomes
fi

git -C cubiomes fetch --quiet origin "$PIN" 2>/dev/null || true
git -C cubiomes checkout --quiet "$PIN"
echo "cubiomes pinned at ${PIN:0:12} ($(git -C cubiomes log -1 --format=%ad --date=short))"

# clang here targets x86_64-pc-windows-msvc, which has no POSIX <sys/time.h>.
# cubiomes/tests.c includes it and uses clock_gettime; this shim supplies both.
mkdir -p cubiomes/compat/sys
cat > cubiomes/compat/sys/time.h <<'SHIM'
/* Shim for building cubiomes with clang targeting x86_64-pc-windows-msvc,
   which ships no POSIX <sys/time.h>. Provides just the monotonic clock that
   tests.c needs, backed by QueryPerformanceCounter. */
#pragma once

#include <time.h>
#include <windows.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

typedef int clockid_t;

static inline int clock_gettime(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    LARGE_INTEGER freq, ctr;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr))
        return -1;
    ts->tv_sec = (time_t)(ctr.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)(((ctr.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
    return 0;
}
SHIM

echo "compat shim written"
echo
echo "next: ./build.sh tools/find.c   then   ./test.sh"
