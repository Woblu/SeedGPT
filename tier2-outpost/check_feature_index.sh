#!/usr/bin/env bash
# Cross-check FeatureIndex against indices that are already known good.
#
# Decoration RNG is seeded with populationSeed + index + 10000*step. A wrong
# index still produces output, just in the wrong places, so the index has to be
# checked against something independent rather than reasoned about.
#
# cubiomes carries a hand-maintained table of {index, step} for the ore features
# (getOreConfig in finders.c), and our ore search is tested against real worlds.
# So if FeatureIndex reproduces those numbers exactly, its answers for features
# nobody has tabulated -- cacti, say -- are trustworthy by the same mechanism.
set -e
cd "$(dirname "$0")/.."

JH="${JAVA_HOME:-/c/Program Files/Java/jdk-21}"
[ -f tier2-outpost/cp.txt ] || { echo "no cp.txt: run gradle printcp first"; exit 2; }

out=$("$JH/bin/java" -cp "tier2-outpost/out;$(cat tier2-outpost/cp.txt)" FeatureIndex ore 2>&1 |
      sed 's/.*STDOUT\]: //' | awk -F'\t' '$1==6 {print $2"\t"$3}')

# name -> index, as cubiomes has them for 1.20+ (the o_*_120 / o_*_118 entries)
expect="0	minecraft:ore_dirt
18	minecraft:ore_diamond
21	minecraft:ore_diamond_buried
23	minecraft:ore_lapis_buried
25	minecraft:ore_copper
27	minecraft:ore_clay
28	minecraft:ore_gold_extra
33	minecraft:ore_emerald"

bad=0
while IFS=$'\t' read -r idx name; do
  got=$(echo "$out" | awk -F'\t' -v n="$name" '$2==n {print $1}')
  if [ "$got" = "$idx" ]; then
    echo "  ok    $name = $idx"
  else
    echo "  FAIL  $name: cubiomes says $idx, the game says ${got:-<missing>}"
    bad=$((bad+1))
  fi
done <<< "$expect"

if [ "$bad" -eq 0 ]; then
  echo "all 8 known-good indices reproduced"
else
  echo "$bad mismatches -- do not trust any index from this tool until resolved"
  exit 1
fi
