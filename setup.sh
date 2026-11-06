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
#
# Windows ONLY. cubiomes/compat is on the include path for every compile, so on
# Linux or macOS this file would shadow the real <sys/time.h> -- which system
# headers include themselves -- and drag <windows.h> into a build that has none.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ;;
  *) rm -rf cubiomes/compat
     echo "non-Windows: no compat shim needed"
     SKIP_SHIM=1 ;;
esac
if [ -z "${SKIP_SHIM:-}" ]; then
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
fi

# Upstream's getFortressPieces takes a buffer bound, stores it in env->nmax and
# never reads it -- so a fortress with more pieces than the caller allocated
# writes past the end of the array. That is most fortresses (84% exceed 64
# pieces), which is why fortress chest loot was unsupported here.
#
# `git checkout <commit>` above does NOT discard modifications to tracked files,
# so the working tree is reset explicitly first: without it a second run of this
# script hits an already-patched finders.c, `git apply` fails, and set -e kills
# the setup. Restoring then applying makes re-running a no-op, which is what a
# setup script has to be.
git -C cubiomes checkout --quiet -- .
git -C cubiomes apply ../patches/fortress-piece-bound.patch
echo "fortress piece-bound patch applied"

# Stronghold pieces with their REAL Y. Upstream pins the start piece at Y=64 and
# says so in the header: the height costs extra RNG bookkeeping and loot does not
# depend on it. But "is this stronghold exposed" is a question about Y and
# nothing else, so the engine could not answer it at all.
#
# The patch adds getStrongholdPiecesOpt(..., adjustY) and leaves
# getStrongholdPieces bit-identical, so the validated loot and eye-count paths
# are untouched. Without this the stronghold conditions in src/query.c do not
# compile, which is why it belongs here rather than in a developer's working
# tree -- cubiomes/ is gitignored and the checkout above would discard it.
git -C cubiomes apply ../patches/stronghold-accurate-y.patch
echo "stronghold accurate-Y patch applied"

# The filler-corridor rule upstream gates at 1.17; the evidence says 1.18.
#
# extendStrongholdPiece tries filler-corridor lengths i = 2,1,0 and upstream
# stops at i=1 from MC_1_17 on, so a 1.17 stronghold gets fewer corridors, is
# shorter, and its roof lands lower. That is testable, because exposed
# strongholds are a known 1.17.1 find: against DylanDC14's 12 published seeds
# (8 village, 3 mansion, 1 ocean, all verified in game at 1.17.1) upstream's
# gate confirms 3 of 12, and moving it to 1.18 confirms 8 of 12 -- five seeds
# flip from wrong to right and none flip the other way. Two examples, same
# seed, same coordinates:
#     upstream 1.17   84 -2220  pieces=121  maxY=56  surface=61  depth= 5
#     gate at 1.18    84 -2220  pieces=189  maxY=64  surface=61  depth=-3 EXPOSED
# The remaining 4 sit 1-5 blocks under our SMOOTHED surface estimate, which is
# mapApproxHeight for anything below 1.18 and provably cannot see a sharp dip --
# a separate limit, not this gate.
#
# 1.16 and 1.18+ are bit-identical before and after (verified by re-running the
# same seeds), so only 1.17 moves. NOT independently confirmed against a real
# 1.17 server: 1.17 stronghold piece lists change, so 1.17 eye counts and
# stronghold loot may shift with it. The validated paths are 1.16.5.
git -C cubiomes apply ../patches/stronghold-1-17-filler-corridor.patch
echo "stronghold 1.17 filler-corridor patch applied"

# generateRegion / generateNetherRegion memoise noise columns in a W x H grid of
# cells (W along x, H along z) but index it as relCellX * W + relCellZ, which is
# only right when W == H. A region wider than it is tall then writes past the
# end of the malloc'd buffer; one taller than wide makes two cells share a slot,
# and since the bitset marks the slot filled, the second cell silently reuses
# the first one's noise and comes back with the wrong heights.
#
# Every caller upstream passes square regions, which is why it never shows
# there. exactFootprint does not: its region is the chunk span of
# [centre +- within] on each axis, which is rectangular whenever the centre is
# not chunk-aligned -- within=20 at (0,8) is 4 x 3 chunks, and writes cell 284
# of a 221-cell buffer. Exact terrain height, exposed ore, cave_below and the
# stronghold depth all go through it. The fix is the stride: relCellX * H.
git -C cubiomes apply ../patches/terrainnoise-region-stride.patch
echo "terrainnoise region-stride patch applied"

# Two loot-table fixes, applied as a table is loaded (on a private copy: the
# compiled tables are static and shared). Both were found by comparing chests
# on a real 1.21.1 server (tier3-java/check_loot_seed.py) and both shift every
# roll after the item they touch, so they change more than one item:
#  - From 1.21 every vanilla enchant_randomly without its own enchantment list
#    says "options": "#minecraft:on_random_loot". The library serves the first
#    1.21.x releases with tables compiled from 1.20, which build the 1.20
#    default list instead: a different enchantment for the same roll, and a
#    skipped level roll when it has one level. A desert pyramid chest held an
#    emerald where the 1.20 table rolled an enchanted golden apple.
#  - An outpost goat horn's set_instrument draws one nextInt(4) for its sound,
#    and the compiled tables leave the function out (1.19+): an outpost chest
#    got two sentry trims the real one did not.
git -C cubiomes apply ../patches/loot-fixups.patch
echo "loot-table fixes applied"

# Chest POSITIONS (the loot seeds were right): a desert pyramid fills its four
# chests in an order that depends on the piece's random facing, which upstream
# ignored, pairing three of four seeds with the wrong chest; and the outpost
# watchtower's chest is at template (9, 14, 10), not (10, _, 10), so the
# reported chest was one block off. Both checked on a real 1.21.1 server by
# tier3-java/check_loot_seed.py: every chest now holds the seed predicted for
# it, where it was predicted.
git -C cubiomes apply ../patches/structure-chest-positions.patch
echo "structure chest-position patch applied"
echo
echo "next: ./build.sh tools/find.c   then   ./test.sh"
