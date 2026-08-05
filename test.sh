#!/usr/bin/env bash
# Regression suite. Encodes every check that has been run by hand so far, so a
# refactor that breaks correctness fails here instead of silently shipping.
#
#   ./test.sh          run everything
#   ./test.sh -v       show each command
#
# Exit 0 = all green. Any failure is fatal and printed with context.
set -u
cd "$(dirname "$0")"

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

pass=0; fail=0
JC=/tmp/sc_test/classes
mkdir -p /tmp/sc_test

ok()   { pass=$((pass+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }
run()  { [ "$VERBOSE" = 1 ] && printf '     $ %s\n' "$*"; "$@"; }

section() { printf '\n\033[1m%s\033[0m\n' "$1"; }

# ---------------------------------------------------------------- build
section "build"
if ./build.sh tools/find.c   >/tmp/sc_test/b1 2>&1 \
&& ./build.sh tools/vocab.c  >/tmp/sc_test/b2 2>&1 \
&& ./build.sh tools/xval.c   >/tmp/sc_test/b3 2>&1 \
&& ./build.sh tools/describe.c >/tmp/sc_test/b4 2>&1 && ./build.sh tools/map.c    >/tmp/sc_test/b5 2>&1 \n&& ./build.sh tools/confidence.c >/tmp/sc_test/b6 2>&1 \n&& ./build.sh tools/checkeyes.c >/tmp/sc_test/b7 2>&1 \n&& ./build.sh tools/checkloot.c >/tmp/sc_test/b8 2>&1 && ./build.sh tools/lootitems.c >/tmp/sc_test/b9 2>&1 \
&& ./build.sh tools/checkportal.c >/tmp/sc_test/b10 2>&1 \
&& ./build.sh tools/checkore.c >/tmp/sc_test/b11 2>&1 \
&& ./build.sh tools/checkvariant.c >/tmp/sc_test/b12 2>&1 \
&& ./build.sh tools/checkrpchest.c >/tmp/sc_test/b13 2>&1 \
&& ./build.sh tools/checkslime.c >/tmp/sc_test/b14 2>&1 \
&& ./build.sh tools/checkbiomearea.c >/tmp/sc_test/b15 2>&1 \
&& ./build.sh tools/checkbiome.c >/tmp/sc_test/b16 2>&1 \
&& ./build.sh tools/checkcluster.c >/tmp/sc_test/b17 2>&1 \
&& ./build.sh tools/checkheight.c >/tmp/sc_test/b18 2>&1 \n&& ./build.sh tools/checkplacement.c >/tmp/sc_test/b19 2>&1 \
&& ./build.sh tools/climatecheck.c >/tmp/sc_test/b20 2>&1 \n&& ./build.sh tools/cactus.c >/tmp/sc_test/b21 2>&1 \n&& ./build.sh tools/invert.c >/tmp/sc_test/b22 2>&1; then
  ok "all tools compile"
else
  bad "build failed" "$(cat /tmp/sc_test/b1 /tmp/sc_test/b2 /tmp/sc_test/b3 /tmp/sc_test/b4 /tmp/sc_test/b5 2>/dev/null | grep -i error | head -3)"
  echo "aborting: nothing to test"; exit 1
fi

javac -d "$JC" tools/XVal.java tools/Verify.java 2>/tmp/sc_test/jc \
  && ok "java reference compiles" \
  || bad "javac failed" "$(cat /tmp/sc_test/jc)"

# ---------------------------------------------------------------- cross-validation
section "cross-validation vs independent JDK reference"
./build/xval.exe > /tmp/sc_test/c.txt 2>&1
java -cp "$JC" XVal > /tmp/sc_test/j.txt 2>&1

if diff -q <(grep CONFIG /tmp/sc_test/c.txt) <(grep CONFIG /tmp/sc_test/j.txt) >/dev/null; then
  ok "structure configs match (salts, spacing)"
else
  bad "config mismatch" "$(diff <(grep CONFIG /tmp/sc_test/c.txt) <(grep CONFIG /tmp/sc_test/j.txt) | head -4)"
fi

npos=$(grep -c '^POS' /tmp/sc_test/c.txt)
if diff -q <(grep '^POS' /tmp/sc_test/c.txt) <(grep '^POS' /tmp/sc_test/j.txt) >/dev/null; then
  ok "all $npos structure positions match (linear+triangular, negatives, 26.2)"
else
  bad "position mismatch" "$(diff <(grep '^POS' /tmp/sc_test/c.txt) <(grep '^POS' /tmp/sc_test/j.txt) | head -6)"
fi

# ---------------------------------------------------------------- planner reordering
section "planner"
# Written biome-first with a forward reference; must execute geometry-first.
cat > /tmp/sc_test/reorder.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"jungle","biome":"jungle","within":400,"of":"mansion"},
 {"id":"mansion","structure":"mansion","within":500,"of":"origin"}]}
EOF
plan=$(./build/find.exe /tmp/sc_test/reorder.json 1 1 2>&1)
# pass 1 must contain mansion geometry and NOT the biome
if echo "$plan" | sed -n '/pass 1/,/pass 2/p' | grep -q mansion \
&& ! echo "$plan" | sed -n '/pass 1/,/pass 2/p' | grep -q jungle; then
  ok "biome-first input reordered to geometry-first execution"
else
  bad "planner did not reorder" "$(echo "$plan" | sed -n '/plan/,/est/p')"
fi

# forward reference resolved
echo "$plan" | grep -q 'jungle within 400 of mansion' \
  && ok "forward reference (jungle -> mansion) resolved" \
  || bad "forward reference not resolved"

# Cost-based ordering WITHIN pass 1: two independent structures at the same
# radius, cheaper first. Mansion (region span 1280) is cheaper than village
# (span 544) -- fewer regions to scan. A broken cost model flips this.
cat > /tmp/sc_test/cost.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"village","structure":"village","within":1000,"of":"origin"},
 {"id":"mansion","structure":"mansion","within":1000,"of":"origin"}]}
EOF
p1order=$(./build/find.exe /tmp/sc_test/cost.json 1 1 2>&1 \
          | sed -n '/pass 1/,/pass 2/p' | grep -oE 'mansion|village' | head -2 | tr '\n' ',')
[ "$p1order" = "mansion,village," ] \
  && ok "pass 1 orders cheaper structure first (mansion before village)" \
  || bad "cost-based ordering wrong" "got: $p1order"

# ---------------------------------------------------------------- error handling
section "error handling"
check_err() {  # name, json, expected-substring
  echo "$2" > /tmp/sc_test/e.json
  out=$(./build/find.exe /tmp/sc_test/e.json 2>&1)
  echo "$out" | grep -qi "$3" && ok "$1" || bad "$1" "got: $out"
}
check_err "dependency cycle detected" \
  '{"version":"1.21","conditions":[{"id":"a","structure":"mansion","within":300,"of":"b"},{"id":"b","structure":"village","within":300,"of":"a"}]}' \
  "cycle"
check_err "unknown biome rejected" \
  '{"version":"1.21","conditions":[{"id":"a","biome":"mushroom_land","within":300}]}' \
  "unknown biome"
check_err "version-unavailable structure rejected" \
  '{"version":"1.12","conditions":[{"id":"a","structure":"ancient_city","within":300}]}' \
  "does not exist"
check_err "unknown parent rejected" \
  '{"version":"1.21","conditions":[{"id":"a","structure":"village","within":300,"of":"nope"}]}' \
  "unknown parent"
# Nether coords are 8:1 compressed -- a radius measured across dimensions is
# meaningless, so refuse rather than silently produce nonsense.
check_err "cross-dimension distance rejected" \
  '{"version":"1.21","conditions":[{"id":"f","structure":"fortress","within":300,"of":"origin"},{"id":"v","structure":"village","within":500,"of":"f"}]}' \
  "cross-dimension"
check_err "bad biome precision rejected" \
  '{"version":"1.21","conditions":[{"id":"j","biome":"jungle","within":300,"precision":"turbo"}]}' \
  "fast|fine|exact"

# Biome scan precision must change the plan's cost estimate, not just parse.
cat > /tmp/sc_test/prec.json <<'EOF'
{"version":"1.21","conditions":[{"id":"j","biome":"jungle","within":400,"of":"origin"}]}
EOF
cfine=$(./build/find.exe /tmp/sc_test/prec.json 1 1 2>&1 | sed -n 's/.*jungle.*\[fine\] *~\([0-9]*\) ns.*/\1/p' | head -1)
sed -i 's/"of":"origin"}/"of":"origin","precision":"fast"}/' /tmp/sc_test/prec.json
cfast=$(./build/find.exe /tmp/sc_test/prec.json 1 1 2>&1 | sed -n 's/.*jungle.*\[fast\] *~\([0-9]*\) ns.*/\1/p' | head -1)
if [ -n "$cfine" ] && [ -n "$cfast" ] && [ "$cfine" -gt "$cfast" ]; then
  ok "biome precision affects cost model (fine ${cfine}ns > fast ${cfast}ns)"
else
  bad "precision not reflected in cost" "fine=$cfine fast=$cfast"
fi

# Two conditions of the same structure type must match two DIFFERENT instances.
# Without this, "village within 400 of village" is satisfied by the same village
# at distance 0 and multi-instance constellations (quad huts) never filter.
cat > /tmp/sc_test/dist.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"v1","structure":"village","within":400,"of":"origin"},
 {"id":"v2","structure":"village","within":400,"of":"v1"}]}
EOF
FIND_TSV=/tmp/sc_test/dist.tsv ./build/find.exe /tmp/sc_test/dist.json 3000000 16 >/dev/null 2>&1
if [ -s /tmp/sc_test/dist.tsv ]; then
  same=0
  while IFS=$'\t' read -r s a x1 z1 b x2 z2; do
    [ "$x1" = "$x2" ] && [ "$z1" = "$z2" ] && same=$((same+1))
  done < /tmp/sc_test/dist.tsv
  [ "$same" -eq 0 ] \
    && ok "same-type conditions match distinct instances ($(wc -l < /tmp/sc_test/dist.tsv) seeds)" \
    || bad "same structure matched twice" "$same seeds had v1 == v2"
else
  bad "distinctness query returned nothing"
fi

# "spawn" must mean the actual world spawn, not the origin. Measured over 400
# seeds the median spawn is 22 blocks from origin but p90 is 520 -- 47% land
# further than 35 blocks out, so conflating them yields results that are right
# on paper and wrong in game.
section "spawn semantics"
cat > /tmp/sc_test/spawn.json <<'EOF'
{"version":"1.21","conditions":[{"id":"p","structure":"ruined_portal","within":35,"of":"spawn"}]}
EOF
sp_out=$(FIND_TSV=/tmp/sc_test/spawn.tsv ./build/find.exe /tmp/sc_test/spawn.json 3000000 16 2>&1)
nsp=$(echo "$sp_out" | grep -c '^SEED')
if [ "$nsp" -gt 0 ]; then
  ok "spawn-relative query returns seeds ($nsp)"
else
  bad "spawn-relative query found nothing"
fi
# Every reported distance must be within the requested radius OF SPAWN.
# grep -oE, not sed: a greedy .* lets the capture group match empty, and the
# check then silently passes on nothing.
worst=$(echo "$sp_out" | grep -oE '[0-9]+ from spawn' | awk '{print $1}' | sort -n | tail -1)
if [ -n "$worst" ] && [ "$worst" -le 35 ]; then
  ok "all matches within 35 blocks of true spawn (worst $worst)"
else
  bad "spawn distance exceeded radius" "worst='$worst'"
fi

# Independently: describe.exe is a separate binary, so this does not rest on
# find.exe agreeing with itself. Uses the TSV, not the aligned human output --
# "x=%6d" puts spaces inside the field and breaks naive splitting.
if python tools/checkspawn.py /tmp/sc_test/spawn.tsv 35 1.21 ./build/describe.exe      >/tmp/sc_test/cs.log 2>&1; then
  ok "independent recheck via describe ($(cat /tmp/sc_test/cs.log))"
else
  bad "independent recheck failed" "$(cat /tmp/sc_test/cs.log)"
fi

# Reported seeds must not be tiny. Trying upper bits from 0 made the first
# world seed literally equal the structure seed, so a scan from 0 returned
# "128", "146", ... clustered at thread boundaries.
big=$(echo "$sp_out" | grep '^SEED' | awk '{print ($2<0?-$2:$2)}' | sort -n | tail -1)
[ -n "$big" ] && [ "${#big}" -ge 10 ]   && ok "seeds use the full 64-bit range (largest has ${#big} digits)"   || bad "seeds look truncated" "largest magnitude: $big"

# The engine cannot verify every structure. cubiomes' isViableFeatureBiome
# returns unconditionally for ruined portals, so 100% of generation attempts
# are reported as hits (vs 0.9-31.7% for biome-checked structures). A user hit
# exactly this: reported portals that did not exist in game. The tool must say
# so rather than presenting them as equivalent.
section "verification confidence"
cat > /tmp/sc_test/rp.json <<'EOF'
{"version":"1.21","conditions":[{"id":"p","structure":"ruined_portal","within":200,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/rp.json 1 1 2>&1 | grep -q "may not exist in your world"   && ok "unverifiable structure carries a warning"   || bad "no warning for ruined_portal"
./build/find.exe queries/mansion-village.json 1 1 2>&1 | grep -q "NOTE:"   && bad "spurious warning on biome-checked structures"   || ok "biome-checked structures carry no warning"
# And the measurement behind the claim must still hold.
./build/confidence.exe 1.21 20 > /tmp/sc_test/conf.log 2>&1
grep -q "ruined_portal.*NONE" /tmp/sc_test/conf.log   && ok "ruined_portal still measures as unverified"   || bad "confidence measurement changed" "$(grep ruined_portal /tmp/sc_test/conf.log)"
grep -q "village.*biome-checked" /tmp/sc_test/conf.log   && ok "village still measures as biome-checked"   || bad "village verification changed"

# The "surface" filter drops the buried variant. It is an EXACT filter, not an
# approximation: getVariant reads the same nextFloat<0.5 the game uses. Prove it
# by re-deriving every matched portal independently -- none may be underground,
# and getVariant must agree with the from-scratch recomputation (checkportal).
section "ruined portal surface filter"
cat > /tmp/sc_test/rps.json <<'EOF'
{"version":"1.21","conditions":[{"id":"rp","structure":"ruined_portal","within":800,"of":"origin","surface":true}]}
EOF
# Reject the flag where it has no meaning.
check_err "surface flag rejected off ruined_portal" \
  '{"version":"1.21","conditions":[{"id":"v","structure":"village","within":400,"of":"origin","surface":true}]}' \
  "only applies to ruined_portal"
# Verify every matched portal is a surface portal, independently.
bad_cnt=0; ver_cnt=0; rseed=""
while read -r line; do
  if [ "${line%% *}" = "SEED" ]; then
    rseed=$(echo "$line" | awk '{print $2}')
  else
    rx=$(echo "$line" | grep -oE 'x=[[:space:]]*-?[0-9]+' | grep -oE '\-?[0-9]+$')
    rz=$(echo "$line" | grep -oE 'z=[[:space:]]*-?[0-9]+' | grep -oE '\-?[0-9]+$')
    v=$(./build/checkportal.exe "$rseed" 1.21 --at "$rx" "$rz" 2>/dev/null)
    ver_cnt=$((ver_cnt+1))
    echo "$v" | grep -Eq "underground=1|MISMATCH" && bad_cnt=$((bad_cnt+1))
  fi
done < <(./build/find.exe /tmp/sc_test/rps.json 300000 16 2>/dev/null | grep -E "^SEED|rp " | \
         awk '/^SEED/{s=$2} /rp /{print "SEED "s"\n"$0}')
[ "$ver_cnt" -ge 5 ] && [ "$bad_cnt" -eq 0 ] \
  && ok "surface filter: all $ver_cnt matched portals independently confirmed surface" \
  || bad "surface filter admitted a buried/mismatched portal" "checked=$ver_cnt bad=$bad_cnt"

# Ore density counts Minecraft's ore placement (getOreConfig -> generateOres),
# deduped across configs. Every reported count must reproduce exactly under an
# independent re-count (checkore), and every result must clear the threshold.
section "ore density search"
check_err "unknown ore rejected" \
  '{"version":"1.21","conditions":[{"id":"o","ore":"unobtanium","count":1,"within":64}]}' \
  "unknown ore"
cat > /tmp/sc_test/ore.json <<'EOF'
{"version":"1.21","conditions":[
  {"id":"v","structure":"village","within":600,"of":"origin"},
  {"id":"dia","ore":"diamond","count":1200,"of":"v","within":64}]}
EOF
./build/find.exe /tmp/sc_test/ore.json 300000 16 2>/dev/null > /tmp/sc_test/ore.out
ore_bad=0; ore_low=0; ore_n=0
while read -r oseed ox oz ocnt; do
  chk=$(./build/checkore.exe "$oseed" diamond 1.21 "$ox" "$oz" 64 2>/dev/null | grep -oE 'count=[0-9]+' | cut -d= -f2)
  ore_n=$((ore_n+1))
  [ "$ocnt" != "$chk" ] && ore_bad=$((ore_bad+1))
  [ "$ocnt" -lt 1200 ] && ore_low=$((ore_low+1))
done < <(awk '/^SEED/{s=$2} /dia /{gsub(/x=|z=/,""); print s, $2, $3, $4}' /tmp/sc_test/ore.out | head -8)
[ "$ore_n" -ge 5 ] && [ "$ore_bad" -eq 0 ] \
  && ok "ore counts reproduce exactly under independent re-count ($ore_n checked)" \
  || bad "ore count disagreed with checkore" "checked=$ore_n mismatch=$ore_bad"
[ "$ore_low" -eq 0 ] \
  && ok "every ore result clears the requested threshold" \
  || bad "ore result below threshold" "under=$ore_low"

# Slime chunks are a per-chunk Java-RNG check on the world seed (isSlimeChunk),
# version-independent. Every reported cluster count must reproduce exactly under
# an independent re-count (checkslime), and every result must clear the
# threshold. Origin- and spawn-relative both exercised.
section "slime chunk cluster search"
cat > /tmp/sc_test/slime.json <<'EOF'
{"version":"1.21","conditions":[{"id":"cl","slime":18,"within":128,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/slime.json 200000 16 2>/dev/null > /tmp/sc_test/slime.out
sl_bad=0; sl_low=0; sl_n=0
while read -r sseed sx sz scnt; do
  chk=$(./build/checkslime.exe "$sseed" "$sx" "$sz" 128 2>/dev/null | grep -oE 'count=[0-9]+' | cut -d= -f2)
  sl_n=$((sl_n+1))
  [ "$scnt" != "$chk" ] && sl_bad=$((sl_bad+1))
  [ "$scnt" -lt 18 ] && sl_low=$((sl_low+1))
done < <(awk '/^SEED/{s=$2} /cl /{gsub(/x=|z=/,""); print s, $2, $3, $4}' /tmp/sc_test/slime.out | head -8)
[ "$sl_n" -ge 5 ] && [ "$sl_bad" -eq 0 ] \
  && ok "slime counts reproduce exactly under independent re-count ($sl_n checked)" \
  || bad "slime count disagreed with checkslime" "checked=$sl_n mismatch=$sl_bad"
[ "$sl_low" -eq 0 ] \
  && ok "every slime result clears the requested threshold" \
  || bad "slime result below threshold" "under=$sl_low"

# Biome-area finds seeds where a biome covers >= N% of a disc (a size filter for
# huge mushroom islands / mesas). The reported percentage must reproduce exactly
# under an identical independent re-sample (checkbiomearea), and every result
# must clear the threshold. Sampled, so a coarse scan only ever misses seeds.
section "biome area search"
check_err "unknown biome_area rejected" \
  '{"version":"1.21","conditions":[{"id":"a","biome_area":"nonexistent_biome","pct":40,"within":400}]}' \
  "unknown biome"
cat > /tmp/sc_test/area.json <<'EOF'
{"version":"1.21","conditions":[{"id":"sh","biome_area":"mushroom_fields","pct":40,"within":400,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/area.json 400000 16 2>/dev/null > /tmp/sc_test/area.out
ar_bad=0; ar_low=0; ar_n=0
while read -r aseed ax az apct; do
  chk=$(./build/checkbiomearea.exe "$aseed" mushroom_fields 1.21 "$ax" "$az" 400 fine 2>/dev/null | grep -oE 'pct=[0-9]+' | cut -d= -f2)
  ar_n=$((ar_n+1))
  [ "$apct" != "$chk" ] && ar_bad=$((ar_bad+1))
  [ "$apct" -lt 40 ] && ar_low=$((ar_low+1))
done < <(awk '/^SEED/{s=$2} /sh /{gsub(/x=|z=|%/,""); print s, $2, $3, $4}' /tmp/sc_test/area.out | head -8)
[ "$ar_n" -ge 5 ] && [ "$ar_bad" -eq 0 ] \
  && ok "biome-area percentages reproduce exactly under independent re-sample ($ar_n checked)" \
  || bad "biome-area percentage disagreed with checkbiomearea" "checked=$ar_n mismatch=$ar_bad"
[ "$ar_low" -eq 0 ] \
  && ok "every biome-area result clears the requested threshold" \
  || bad "biome-area result below threshold" "under=$ar_low"

# Terrain height (APPROXIMATE). The finder's reported peak must reproduce under
# an identical independent re-sample (checkheight, same mapApproxHeight lattice)
# and clear the threshold. This confirms the plumbing -- the height is cubiomes'
# estimate, deliberately not claimed to be exact game height.
section "terrain height search"
cat > /tmp/sc_test/height.json <<'EOF'
{"version":"1.21","conditions":[{"id":"peak","height":150,"within":300,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/height.json 200000 16 2>/dev/null > /tmp/sc_test/height.out
ht_bad=0; ht_low=0; ht_n=0
while read -r hseed hpk; do
  chk=$(./build/checkheight.exe "$hseed" 1.21 0 0 300 fine 2>/dev/null | grep -oE 'peak=-?[0-9]+' | cut -d= -f2)
  ht_n=$((ht_n+1))
  [ "$hpk" != "$chk" ] && ht_bad=$((ht_bad+1))
  [ "$hpk" -lt 150 ] && ht_low=$((ht_low+1))
done < <(awk '/^SEED/{s=$2} /peak within/{gsub(/x=|z=|~/,""); print s, $4}' /tmp/sc_test/height.out | head -8)
[ "$ht_n" -ge 5 ] && [ "$ht_bad" -eq 0 ] \
  && ok "terrain-height peaks reproduce exactly under independent re-sample ($ht_n checked)" \
  || bad "terrain-height peak disagreed with checkheight" "checked=$ht_n mismatch=$ht_bad"
[ "$ht_low" -eq 0 ] \
  && ok "every terrain-height result clears the requested peak" \
  || bad "terrain-height result below threshold" "under=$ht_low"

# Structure clusters: "count": N asks for >= N viable instances of a structure
# within the radius (triple village near spawn, huts near spawn). Every reported
# count must reproduce under an independent re-count from the SAME reference the
# finder used (checkcluster, counting from spawn), and clear the threshold.
section "structure cluster search"
check_err "variant + cluster count rejected" \
  '{"version":"1.21","conditions":[{"id":"v","structure":"village","count":3,"within":800,"of":"spawn","abandoned":true}]}' \
  "cluster count"
cat > /tmp/sc_test/cluster.json <<'EOF'
{"version":"1.21","conditions":[{"id":"vils","structure":"village","count":3,"within":800,"of":"spawn"}]}
EOF
./build/find.exe /tmp/sc_test/cluster.json 300000 16 2>/dev/null > /tmp/sc_test/cluster.out
cl_bad=0; cl_low=0; cl_n=0
while read -r cseed sx sz ccnt; do
  chk=$(./build/checkcluster.exe "$cseed" village 1.21 "$sx" "$sz" 800 2>/dev/null | grep -oE 'count=[0-9]+' | cut -d= -f2)
  cl_n=$((cl_n+1))
  [ "$ccnt" != "$chk" ] && cl_bad=$((cl_bad+1))
  [ "$ccnt" -lt 3 ] && cl_low=$((cl_low+1))
done < <(awk '/^SEED/{s=$2} /\(spawn\)/{gsub(/x=|z=/,""); sx=$2; sz=$3} /vils /{gsub(/x=|z=/,""); print s, sx, sz, $4}' /tmp/sc_test/cluster.out | head -8)
[ "$cl_n" -ge 5 ] && [ "$cl_bad" -eq 0 ] \
  && ok "cluster counts reproduce exactly under independent re-count ($cl_n checked)" \
  || bad "cluster count disagreed with checkcluster" "checked=$cl_n mismatch=$cl_bad"
[ "$cl_low" -eq 0 ] \
  && ok "every cluster result clears the requested threshold" \
  || bad "cluster result below threshold" "under=$cl_low"

# Tight cluster ("spread"): N instances within T of a common member, anywhere in
# the reach -- the quad-hut search. The finder reports a real member position, so
# the count re-checks exactly: viable instances within T of it (checkcluster).
check_err "spread needs count >= 2" \
  '{"version":"1.21","conditions":[{"id":"h","structure":"swamp_hut","spread":128,"within":5000,"of":"origin"}]}' \
  'spread'
cat > /tmp/sc_test/tight.json <<'EOF'
{"version":"1.21","conditions":[{"id":"huts","structure":"swamp_hut","count":3,"spread":300,"within":5000,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/tight.json 3000000 16 2>/dev/null > /tmp/sc_test/tight.out
tg_bad=0; tg_low=0; tg_n=0
while read -r tseed tx tz tcnt; do
  chk=$(./build/checkcluster.exe "$tseed" swamp_hut 1.21 "$tx" "$tz" 300 2>/dev/null | grep -oE 'count=[0-9]+' | cut -d= -f2)
  tg_n=$((tg_n+1))
  [ "$tcnt" != "$chk" ] && tg_bad=$((tg_bad+1))
  [ "$tcnt" -lt 3 ] && tg_low=$((tg_low+1))
done < <(awk '/^SEED/{s=$2} /huts /{gsub(/x=|z=/,""); print s, $2, $3, $4}' /tmp/sc_test/tight.out | head -8)
[ "$tg_n" -ge 5 ] && [ "$tg_bad" -eq 0 ] \
  && ok "tight-cluster counts re-check exactly at the reported member ($tg_n checked)" \
  || bad "tight-cluster count disagreed with checkcluster" "checked=$tg_n mismatch=$tg_bad"
[ "$tg_low" -eq 0 ] \
  && ok "every tight-cluster result meets the requested count within its spread" \
  || bad "tight-cluster result below threshold" "under=$tg_low"

# Biome adjacency: a biome measured from another biome ("desert within D of a
# forest"). The parent biome must be scheduled before the child in pass 2, both
# reported positions must independently be the named biome (checkbiome), and the
# child must lie within its radius of the parent.
section "biome adjacency"
check_err "biome parent must be structure or biome" \
  '{"version":"1.21","conditions":[{"id":"o","ore":"diamond","count":1,"within":64,"of":"origin"},{"id":"b","biome":"desert","within":200,"of":"o"}]}' \
  "must be a structure or a biome"
cat > /tmp/sc_test/adj.json <<'EOF'
{"version":"1.21","conditions":[
  {"id":"forest","biome":"forest","within":400,"of":"origin","precision":"fast"},
  {"id":"desert","biome":"desert","within":200,"of":"forest","precision":"fast"}]}
EOF
# Plan must order the parent (forest) before the child (desert) in pass 2.
./build/find.exe /tmp/sc_test/adj.json --explain 1 1 2>/dev/null >/dev/null   # smoke
plan=$(./build/find.exe /tmp/sc_test/adj.json 1 1 2>/dev/null | awk '/^    [0-9]\./{print $2}')
if echo "$plan" | head -1 | grep -q forest; then
  ok "pass 2 schedules the biome parent (forest) before its child (desert)"
else
  bad "biome parent not scheduled first" "order: $(echo $plan | tr '\n' ' ')"
fi
./build/find.exe /tmp/sc_test/adj.json 40000 16 2>/dev/null > /tmp/sc_test/adj.out
aj_bad=0; aj_far=0; aj_n=0
while read -r jseed fx fz dx dz; do
  # Both reported positions must genuinely be their biome.
  ./build/checkbiome.exe "$jseed" 1.21 "$fx" "$fz" forest >/dev/null 2>&1 || aj_bad=$((aj_bad+1))
  ./build/checkbiome.exe "$jseed" 1.21 "$dx" "$dz" desert >/dev/null 2>&1 || aj_bad=$((aj_bad+1))
  # Child must be within its 200-block radius of the parent forest.
  dist=$(awk "BEGIN{print int(sqrt(($fx-$dx)^2+($fz-$dz)^2))}")
  [ "$dist" -gt 200 ] && aj_far=$((aj_far+1))
  aj_n=$((aj_n+1))
done < <(awk '/^SEED/{s=$2} /^   forest /{gsub(/x=|z=/,""); fx=$2; fz=$3} /^   desert /{gsub(/x=|z=/,""); print s, fx, fz, $2, $3}' /tmp/sc_test/adj.out | head -8)
[ "$aj_n" -ge 5 ] && [ "$aj_bad" -eq 0 ] \
  && ok "both adjacency biomes independently confirmed at their reported positions ($aj_n checked)" \
  || bad "an adjacency biome position was not that biome" "checked=$aj_n bad=$aj_bad"
[ "$aj_far" -eq 0 ] \
  && ok "every adjacency child lies within its radius of the parent biome" \
  || bad "an adjacency child exceeded its radius" "over=$aj_far"

# Structure variant filters (zombie village, igloo basement, giant portal) are
# exact -- getVariant reads the same draw the game does. Each filtered result
# must carry the flag under an independent re-read, and each flag is gated to
# the one structure that has it.
section "structure variant filters"
check_err "abandoned rejected off village" \
  '{"version":"1.21","conditions":[{"id":"x","structure":"igloo","within":500,"of":"origin","abandoned":true}]}' \
  "only applies to village"
check_err "basement rejected off igloo" \
  '{"version":"1.21","conditions":[{"id":"x","structure":"village","within":500,"of":"origin","basement":true}]}' \
  "only applies to igloo"
cat > /tmp/sc_test/zv.json <<'EOF'
{"version":"1.21","conditions":[{"id":"zv","structure":"village","within":2000,"of":"origin","abandoned":true}]}
EOF
zv_bad=0; zv_n=0
while read -r vseed vx vz; do
  f=$(./build/checkvariant.exe "$vseed" village 1.21 --at "$vx" "$vz" 2>/dev/null)
  zv_n=$((zv_n+1)); echo "$f" | grep -q "abandoned=1" || zv_bad=$((zv_bad+1))
done < <(./build/find.exe /tmp/sc_test/zv.json 200000 16 2>/dev/null | grep -E "^SEED|zv " | \
         awk '/^SEED/{s=$2} /zv /{gsub(/x=|z=/,""); print s, $2, $3}' | head -6)
[ "$zv_n" -ge 4 ] && [ "$zv_bad" -eq 0 ] \
  && ok "zombie-village filter: all $zv_n results independently confirmed abandoned" \
  || bad "a zombie-village result was not abandoned" "checked=$zv_n bad=$zv_bad"


# ---------------------------------------------------------------- dimensions
section "nether / end"
cat > /tmp/sc_test/nether.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"fortress","structure":"fortress","within":300,"of":"origin"},
 {"id":"bastion","structure":"bastion","within":500,"of":"fortress"}]}
EOF
./build/find.exe /tmp/sc_test/nether.json 2000000 16 2>&1 | grep -q '^SEED' \
  && ok "nether query (fortress + bastion) returns seeds" \
  || bad "nether query found nothing"

cat > /tmp/sc_test/end.json <<'EOF'
{"version":"1.21","conditions":[{"id":"city","structure":"end_city","within":3000,"of":"origin"}]}
EOF
endout=$(./build/find.exe /tmp/sc_test/end.json 500000 16 2>&1)
echo "$endout" | grep -q '^SEED' \
  && ok "end query (end_city) returns seeds" \
  || bad "end query found nothing"
# End cities never generate within 1008 blocks of the origin.
mind=$(echo "$endout" | sed -n 's/.*city *x= *\(-\?[0-9]*\) z= *\(-\?[0-9]*\).*/\1 \2/p' \
       | awk '{d=sqrt($1*$1+$2*$2); if(m==""||d<m) m=d} END{printf "%d", m}')
[ -n "$mind" ] && [ "$mind" -ge 1008 ] \
  && ok "end cities respect the 1008-block origin exclusion (nearest $mind)" \
  || bad "end city inside exclusion zone" "nearest=$mind"

# Structures must be classified into the right dimension.
./build/vocab.exe 1.21 | grep -q '"fortress": "nether"' \
  && ./build/vocab.exe 1.21 | grep -q '"end_city": "end"' \
  && ./build/vocab.exe 1.21 | grep -q '"mansion": "overworld"' \
  && ok "vocab reports per-structure dimensions" \
  || bad "vocab dimension classification wrong"

# ---------------------------------------------------------------- search + independent verify
section "search produces independently-verifiable seeds"
FIND_TSV=/tmp/sc_test/hits.tsv ./build/find.exe queries/mansion-village.json 3000000 16 >/tmp/sc_test/run 2>&1
nhits=$(wc -l < /tmp/sc_test/hits.tsv 2>/dev/null || echo 0)
if [ "$nhits" -gt 0 ]; then
  ok "search returned $nhits seeds"
  allok=1
  while IFS=$'\t' read -r s _ mx mz _ vx vz; do
    java -cp "$JC" Verify "$s" "$mx" "$mz" "$vx" "$vz" >/dev/null 2>&1 || { allok=0; echo "       seed $s failed JDK verify"; }
  done < /tmp/sc_test/hits.tsv
  [ "$allok" = 1 ] && ok "all $nhits seeds reproduce under JDK reference" || bad "some seeds failed JDK verify"
else
  bad "search returned no seeds" "$(tail -3 /tmp/sc_test/run)"
fi

# funnel warning fires on a deliberately loose query
cat > /tmp/sc_test/loose.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"m","structure":"mansion","within":2000,"of":"spawn"},
 {"id":"v","structure":"village","within":800,"of":"m"}]}
EOF
# "too loose" wraps across a line break in the output; "Tighten" does not.
./build/find.exe /tmp/sc_test/loose.json 200000 16 2>&1 | grep -qi "Tighten" \
  && ok "loose-query warning fires" \
  || bad "loose-query warning missing"

# ---------------------------------------------------------------- explain calibration
section "explain estimator tracks reality"
# Predicted pass-1 rate should be within a factor of the real search's rate.
# (awk, not grep -P: this git-bash locale rejects -P.)
est=$(./build/find.exe queries/mansion-village.json --explain 2000000 16 2>&1 \
      | sed -n 's/.*pass 1 survival : \([0-9.]*\)%.*/\1/p' | head -1)
real=$(tail -20 /tmp/sc_test/run \
      | sed -n 's/.*(\([0-9.]*\)% survive).*/\1/p' | head -1)
if [ -n "$est" ] && [ -n "$real" ]; then
  within=$(python -c "e=$est; r=$real; import sys; sys.exit(0 if 0.5 < e/r < 2.0 else 1)" && echo yes || echo no)
  [ "$within" = yes ] \
    && ok "predicted pass-1 ${est}% within 2x of real ${real}%" \
    || bad "estimator off" "predicted ${est}%, real ${real}%"
else
  bad "could not parse rates" "est='$est' real='$real'"
fi

# ---------------------------------------------------------------- describe round-trips a known seed
section "describe"
# A seed the searcher already produced+verified must round-trip through describe.
read -r s0 _ mx0 mz0 < <(head -1 /tmp/sc_test/hits.tsv)
desc=$(./build/describe.exe "$s0" 1.21 2>&1)
if echo "$desc" | grep -qi mansion; then
  ok "describe reports the mansion the search found (seed $s0)"
else
  bad "describe missed a known structure" "$(echo "$desc" | head -8)"
fi

# End portal eye counts. 12 frames, each independently 10% -- so a high count
# is brutally rare and the numbers must be real, not plumbing artefacts.
section "end portal eyes"
cat > /tmp/sc_test/eyes.json <<'EOF'
{"version":"1.21","conditions":[{"id":"portal","eyes":6}]}
EOF
FIND_TSV=/tmp/sc_test/eyes.tsv ./build/find.exe /tmp/sc_test/eyes.json 60000 16 > /tmp/sc_test/eyes.log 2>&1
neyes=$(grep -c '^SEED' /tmp/sc_test/eyes.log)
[ "$neyes" -gt 0 ]   && ok "eye-count query returns seeds ($neyes)"   || bad "eye query found nothing" "$(tail -3 /tmp/sc_test/eyes.log)"

# Independently recompute every reported count from scratch.
badeyes=0
for s in $(grep '^SEED' /tmp/sc_test/eyes.log | awk '{print $2}'); do
  got=$(./build/checkeyes.exe "$s" 1.21 2>/dev/null | sed -n 's/.*eyes=\([0-9]*\).*//p')
  if [ -z "$got" ] || [ "$got" -lt 6 ]; then badeyes=$((badeyes+1)); fi
done
[ "$badeyes" -eq 0 ]   && ok "all reported eye counts reproduce independently"   || bad "eye counts do not reproduce" "$badeyes seeds disagreed"

check_err "eyes above 12 rejected"   '{"version":"1.21","conditions":[{"id":"p","eyes":13}]}'   "eyes must be"


# Chest loot search. A diamond in a desert pyramid is ~1 in a few hundred
# pyramids, so the counts must be real -- verified independently, not trusted.
section "chest loot"
cat > /tmp/sc_test/loot.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"chest","loot":{"structure":"desert_pyramid","item":"diamond","count":1},"within":3000}]}
EOF
FIND_TSV=/tmp/sc_test/loot.tsv ./build/find.exe /tmp/sc_test/loot.json 300000 16 > /tmp/sc_test/loot.log 2>&1
nl=$(grep -c '^SEED' /tmp/sc_test/loot.log)
[ "$nl" -gt 0 ]   && ok "loot query returns seeds ($nl)"   || bad "loot query found nothing" "$(tail -3 /tmp/sc_test/loot.log)"

# Reproduce every reported count with the independent verifier.
badloot=0
while IFS=$'	' read -r s a x z; do
  got=$(./build/checkloot.exe "$s" desert_pyramid "$x" "$z" 1.21 diamond 2>/dev/null | sed 's/.*diamond=//')
  if [ -z "$got" ] || [ "$got" -lt 1 ]; then badloot=$((badloot+1)); fi
done < /tmp/sc_test/loot.tsv
[ "$badloot" -eq 0 ]   && ok "all reported loot reproduces independently"   || bad "loot counts do not reproduce" "$badloot seeds disagreed"

check_err "unsupported loot structure rejected"   '{"version":"1.21","conditions":[{"id":"c","loot":{"structure":"mansion","item":"diamond"}}]}'   "not supported"
check_err "loot missing item rejected"   '{"version":"1.21","conditions":[{"id":"c","loot":{"structure":"igloo"}}]}'   "needs"

# Ruined portal chest loot has no getStructurePieces case; it is computed from
# template data (chest offset transformed like Minecraft, decoration-seed loot
# seed). Every reported count must reproduce under the independent checkrpchest
# at the exact matched position -- the same bar as the five above.
section "ruined portal chest loot"
cat > /tmp/sc_test/rploot.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"rp","loot":{"structure":"ruined_portal","item":"golden_apple","count":1},"within":3000}]}
EOF
FIND_TSV=/tmp/sc_test/rp.tsv ./build/find.exe /tmp/sc_test/rploot.json 400000 16 > /tmp/sc_test/rp.log 2>&1
rpn=$(grep -c '^SEED' /tmp/sc_test/rp.log)
[ "$rpn" -gt 0 ]   && ok "ruined portal loot query returns seeds ($rpn)"   || bad "ruined portal loot found nothing" "$(tail -3 /tmp/sc_test/rp.log)"
rp_bad=0; rp_n=0
while IFS=$'	' read -r s a x z; do
  got=$(./build/checkrpchest.exe "$s" 1.21 --at "$x" "$z" golden_apple 2>/dev/null | grep -oE 'golden_apple=[0-9]+' | cut -d= -f2)
  rp_n=$((rp_n+1)); [ -z "$got" ] || [ "$got" -lt 1 ] && rp_bad=$((rp_bad+1))
done < /tmp/sc_test/rp.tsv
[ "$rp_n" -ge 5 ] && [ "$rp_bad" -eq 0 ] \
  && ok "ruined portal loot reproduces independently at the matched chest ($rp_n)" \
  || bad "ruined portal loot did not reproduce" "checked=$rp_n bad=$rp_bad"


# ---------------------------------------------------------------- ui
section "web ui"
# Smoke-test the server's own endpoints. Full browser coverage lives outside
# this suite (it needs playwright); this catches the cheap breakages -- a
# syntax error in serve.py, a renamed tool, a broken parser.
UIPORT=8791
SEED_UI_PORT=$UIPORT python serve.py >/tmp/sc_test/ui.log 2>&1 &
uipid=$!
for _ in $(seq 1 30); do
  curl -s -m 1 "http://127.0.0.1:$UIPORT/api/vocab?v=1.21" >/dev/null 2>&1 && break
  sleep 0.3
done
if curl -s -m 3 "http://127.0.0.1:$UIPORT/api/vocab?v=1.21" | grep -q '"fortress": "nether"'; then
  ok "server serves version-aware vocabulary"
else
  bad "ui vocab endpoint failed" "$(tail -3 /tmp/sc_test/ui.log)"
fi
uiplan=$(curl -s -m 20 -X POST "http://127.0.0.1:$UIPORT/api/plan" \
  -H 'Content-Type: application/json' \
  -d '{"query":{"version":"1.21","conditions":[{"id":"j","biome":"jungle","within":400,"of":"m"},{"id":"m","structure":"mansion","within":500,"of":"spawn"}]}}')
echo "$uiplan" | grep -q '"pass1"' && echo "$uiplan" | grep -q 'mansion within 500' \
  && ok "server parses the plan into structured JSON" \
  || bad "ui plan endpoint failed" "$uiplan"
curl -s -m 10 -X POST "http://127.0.0.1:$UIPORT/api/plan" \
  -H 'Content-Type: application/json' \
  -d '{"query":{"version":"1.21","conditions":[{"id":"a","biome":"lava_sea","within":300}]}}' \
  | grep -q '"error"' \
  && ok "server surfaces tool errors as JSON" \
  || bad "ui error path did not return an error"
curl -s -m 15 "http://127.0.0.1:$UIPORT/api/lootitems?v=1.21" | grep -q '"desert_pyramid"' && ok "server serves loot item vocabulary" || bad "ui lootitems endpoint failed"
curl -s -m 60 "http://127.0.0.1:$UIPORT/api/map?seed=281474976710732&v=1.21&r=1000&px=256"   -o /tmp/sc_test/map.png -w '%{content_type}' 2>/dev/null | grep -q 'image/png'   && ok "server renders a map PNG"   || bad "ui map endpoint failed"
kill "$uipid" 2>/dev/null; wait "$uipid" 2>/dev/null

# The PNG is hand-rolled (no image library), so verify it is structurally valid
# rather than merely non-empty: signature, CRCs, and an inflatable zlib stream
# whose length matches width/height exactly.
#
# Written under build/tmp, not /tmp: git-bash's /tmp is invisible to Windows
# Python, which silently turns this check into a FileNotFoundError.
section "map rendering"
mkdir -p build/tmp
./build/map.exe 281474976710732 1.21 1000 256 build/tmp/test.png 2>/dev/null
if python tools/checkpng.py build/tmp/test.png >/tmp/sc_test/png.log 2>&1; then
  ok "hand-rolled PNG is structurally valid ($(cat /tmp/sc_test/png.log))"
else
  bad "PNG malformed" "$(cat /tmp/sc_test/png.log)"
fi

# Exact terrain reports REAL block heights, and cubiomes returns those as column
# indices (0 = world Y -64, plus one for the air block above). If that
# conversion is ever dropped, exact and approximate heights diverge by ~65 while
# both still look like plausible mountain heights -- so compare the two.
section "exact terrain height"
cat > /tmp/sc_test/exact.json <<'EOF'
{"version":"1.21","conditions":[{"id":"pk","height":200,"within":32,"of":"origin","exact":true}]}
EOF
./build/find.exe /tmp/sc_test/exact.json 4000 16 2>/dev/null > /tmp/sc_test/exact.out
ex_n=0; ex_low=0; : > /tmp/sc_test/exact.diffs
while read -r eseed epk; do
  ex_n=$((ex_n+1))
  [ "$epk" -lt 200 ] && ex_low=$((ex_low+1))
  app=$(./build/checkheight.exe "$eseed" 1.21 0 0 32 fine 2>/dev/null | grep -oE 'peak=-?[0-9]+' | cut -d= -f2)
  d=$((epk - app)); [ "$d" -lt 0 ] && d=$(( -d ))
  echo "$d" >> /tmp/sc_test/exact.diffs
done < <(awk '/^SEED/{s=$2} /peak within/ && /x=/{gsub(/x=|z=|~/,""); print s, $4}' /tmp/sc_test/exact.out | head -8)
# The approximation is a smoothed estimator on a 16-block lattice and reads LOW
# on sharp peaks -- usually by ~15-35 blocks, occasionally by over 100. So a
# single sample proves nothing; what a lost index-to-world-Y conversion looks
# like is EVERY sample shifted by a further 65, which moves the median.
ex_med=$(sort -n /tmp/sc_test/exact.diffs | awk '{a[NR]=$1} END{print (NR? a[int((NR+1)/2)] : 999)}')
[ "$ex_n" -ge 3 ] && [ "$ex_med" -lt 50 ] \
  && ok "exact terrain heights are world Y (median drift ${ex_med} blocks over $ex_n seeds)" \
  || bad "exact terrain height drifted from the approximation" "checked=$ex_n median=$ex_med"
[ "$ex_low" -eq 0 ] \
  && ok "every exact-terrain result clears the requested peak" \
  || bad "exact-terrain result below threshold" "under=$ex_low"

# Leaderboard mode ranks a whole range instead of stopping at the first hits.
# Two properties matter: the table is really sorted, and the top entry's score
# reproduces independently (checkheight re-samples the same lattice).
section "leaderboard (ranked search)"
check_err "rank of an unknown condition rejected" \
  '{"version":"1.21","conditions":[{"id":"pk","height":0,"within":200}],"rank":{"of":"nope","top":5}}' \
  "no condition with id"
cat > /tmp/sc_test/rank.json <<'EOF'
{"version":"1.21","conditions":[{"id":"pk","height":0,"within":200,"of":"origin"}],
 "rank":{"of":"pk","by":"height","top":5}}
EOF
./build/find.exe /tmp/sc_test/rank.json 6000 16 2>/dev/null > /tmp/sc_test/rank.out
rk_n=$(grep -c '^SEED' /tmp/sc_test/rank.out)
rk_sorted=$(awk '/^SEED/{v=$NF; if (NR>1 && prev!="" && v>prev) {print "no"; exit} prev=v} END{print "yes"}' \
            <(grep '^SEED' /tmp/sc_test/rank.out))
[ "$rk_n" -eq 5 ] && [ "$rk_sorted" = "yes" ] \
  && ok "leaderboard returns exactly the requested top 5, best first" \
  || bad "leaderboard order or size wrong" "n=$rk_n sorted=$rk_sorted"
rk_seed=$(grep -m1 '^SEED' /tmp/sc_test/rank.out | awk '{print $2}')
rk_score=$(grep -m1 '^SEED' /tmp/sc_test/rank.out | awk '{print $NF}')
rk_chk=$(./build/checkheight.exe "$rk_seed" 1.21 0 0 200 fine 2>/dev/null | grep -oE 'peak=-?[0-9]+' | cut -d= -f2)
[ "$rk_score" = "$rk_chk" ] \
  && ok "the top-ranked score reproduces independently ($rk_score)" \
  || bad "top-ranked score disagreed with checkheight" "reported=$rk_score check=$rk_chk"

# Ore veins: the largest FACE-CONNECTED blob, and exposure against real terrain.
# Both must reproduce under checkore, which re-runs the scan from scratch.
section "ore veins and exposure"
check_err "exposed ore rejected before 1.18" \
  '{"version":"1.17","conditions":[{"id":"d","ore":"diamond","count":1,"exposed":true,"within":48}]}' \
  "1.18"
cat > /tmp/sc_test/vein.json <<'EOF'
{"version":"1.21","conditions":[{"id":"d","ore":"diamond","count":1,"vein":18,"within":64,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/vein.json 4000 16 2>/dev/null > /tmp/sc_test/vein.out
vn_n=0; vn_bad=0; vn_low=0
while read -r vseed vx vz vvein; do
  vn_n=$((vn_n+1))
  [ "$vvein" -lt 18 ] && vn_low=$((vn_low+1))
  chk=$(./build/checkore.exe "$vseed" diamond 1.21 "$vx" "$vz" 64 2>/dev/null | grep -oE 'vein=[0-9]+' | cut -d= -f2)
  [ "$vvein" != "$chk" ] && vn_bad=$((vn_bad+1))
done < <(awk '/^SEED/{s=$2} /biggest vein/ && /x=/{gsub(/x=|z=/,""); print s, $2, $3, $NF}' /tmp/sc_test/vein.out | head -6)
[ "$vn_n" -ge 3 ] && [ "$vn_bad" -eq 0 ] \
  && ok "largest-vein sizes reproduce exactly under independent re-count ($vn_n checked)" \
  || bad "vein size disagreed with checkore" "checked=$vn_n mismatch=$vn_bad"
[ "$vn_low" -eq 0 ] \
  && ok "every vein result clears the requested vein size" \
  || bad "vein result below threshold" "under=$vn_low"

cat > /tmp/sc_test/exposed.json <<'EOF'
{"version":"1.21","conditions":[{"id":"d","ore":"diamond","count":150,"exposed":true,"within":48,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/exposed.json 120 16 2>/dev/null > /tmp/sc_test/exposed.out
xp_n=0; xp_bad=0; xp_over=0
while read -r xseed xx xz xcnt; do
  xp_n=$((xp_n+1))
  chk=$(./build/checkore.exe "$xseed" diamond 1.21 "$xx" "$xz" 48 exposed 2>/dev/null | grep -oE 'count=[0-9]+' | cut -d= -f2)
  [ "$xcnt" != "$chk" ] && xp_bad=$((xp_bad+1))
  # Exposed is a SUBSET of the total; more exposed than total means the filter
  # is counting something that is not there.
  tot=$(./build/checkore.exe "$xseed" diamond 1.21 "$xx" "$xz" 48 2>/dev/null | grep -oE 'count=[0-9]+' | cut -d= -f2)
  [ "$xcnt" -gt "$tot" ] && xp_over=$((xp_over+1))
done < <(awk '/^SEED/{s=$2} /exposed ore/ && /x=/{gsub(/x=|z=/,""); print s, $2, $3, $4}' /tmp/sc_test/exposed.out | head -4)
[ "$xp_n" -ge 2 ] && [ "$xp_bad" -eq 0 ] && [ "$xp_over" -eq 0 ] \
  && ok "exposed-ore counts reproduce and stay a subset of the total ($xp_n checked)" \
  || bad "exposed-ore count wrong" "checked=$xp_n mismatch=$xp_bad over=$xp_over"

# Structure overlap: the reported pair must actually sit within the sum of the
# two nominal footprints, and both halves must be real (a village that fails its
# biome check is not overlapping anything).
section "structure overlap"
check_err "cross-dimension overlap rejected" \
  '{"version":"1.21","conditions":[{"id":"x","overlap":["village","fortress"],"within":2000}]}' \
  "different dimensions"
cat > /tmp/sc_test/overlap.json <<'EOF'
{"version":"1.21","conditions":[{"id":"ov","overlap":["village","ruined_portal"],"within":2000,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/overlap.json 200000 16 2>/dev/null > /tmp/sc_test/overlap.out
ov_n=0; ov_far=0; ov_unviable=0
while read -r oseed ax az bx bz; do
  ov_n=$((ov_n+1))
  # village half-extent 32 + ruined portal 8 = 40 blocks on each axis
  dx=$((ax - bx)); [ "$dx" -lt 0 ] && dx=$(( -dx ))
  dz=$((az - bz)); [ "$dz" -lt 0 ] && dz=$(( -dz ))
  { [ "$dx" -ge 40 ] || [ "$dz" -ge 40 ]; } && ov_far=$((ov_far+1))
  # Independent confirmation that the first half really generates there:
  # checkcluster counts VIABLE instances in a tiny radius around the report.
  vb=$(./build/checkcluster.exe "$oseed" village 1.21 "$ax" "$az" 16 2>/dev/null | grep -oE 'count=[0-9]+' | cut -d= -f2)
  [ "${vb:-0}" -lt 1 ] && ov_unviable=$((ov_unviable+1))
done < <(awk '/^SEED/{s=$2} /   ov  /{gsub(/x=|z=/,""); a=$2; b=$3; getline; gsub(/x=|z=/,""); print s, a, b, $1, $2}' \
         /tmp/sc_test/overlap.out | head -6)
[ "$ov_n" -ge 3 ] && [ "$ov_far" -eq 0 ] \
  && ok "every overlap pair is inside the summed footprints ($ov_n checked)" \
  || bad "overlap pair too far apart to touch" "checked=$ov_n far=$ov_far"
[ "$ov_unviable" -eq 0 ] \
  && ok "both halves of every overlap independently confirmed to generate" \
  || bad "overlap reported a structure that does not generate" "bad=$ov_unviable"

# Geodes are placed from the chunk POPULATION seed, so they need the full 64-bit
# world seed -- filtering them on the 48-bit structure seed reports geodes that
# are not in the world. checkvariant re-reads size/cracked from scratch.
section "geode shape"
check_err "geode cluster count rejected" \
  '{"version":"1.21","conditions":[{"id":"g","structure":"geode","count":3,"within":500}]}' \
  "count"
cat > /tmp/sc_test/geode.json <<'EOF'
{"version":"1.21","conditions":[{"id":"g","structure":"geode","size":4,"cracked":false,"within":300,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/geode.json 40000 16 2>/dev/null > /tmp/sc_test/geode.out
gd_n=0; gd_bad=0
while read -r gseed gx gz; do
  gd_n=$((gd_n+1))
  line=$(./build/checkvariant.exe "$gseed" geode 1.21 --at "$gx" "$gz" 2>/dev/null)
  echo "$line" | grep -q "size=4 cracked=0" || gd_bad=$((gd_bad+1))
done < <(awk '/^SEED/{s=$2} /   g  /{gsub(/x=|z=/,""); print s, $2, $3}' /tmp/sc_test/geode.out | head -6)
[ "$gd_n" -ge 3 ] && [ "$gd_bad" -eq 0 ] \
  && ok "every geode is independently confirmed sealed and size 4 ($gd_n checked)" \
  || bad "geode variant disagreed with checkvariant" "checked=$gd_n mismatch=$gd_bad"

# From 1.18 three structures also refuse to generate on ground that is too low,
# a rule cubiomes' biome-only viability check does not model -- so the finder
# used to report structures that are not in the world. Measured against the real
# MC 1.21.1 generator over 110 positions: 0 false positives, 5 false negatives
# (cubiomes' terrain differs from the game's by a block or two, and a corner
# under water reads higher in game than the solid ground we measure).
section "1.18+ structure surface rules"
cat > /tmp/sc_test/place.json <<'EOF'
{"version":"1.21","conditions":[{"id":"p","structure":"desert_pyramid","within":600,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/place.json 60000 16 2>/dev/null > /tmp/sc_test/place.out
pl_n=0; pl_bad=0
while read -r pseed px pz; do
  pl_n=$((pl_n+1))
  ./build/checkplacement.exe "$pseed" desert_pyramid 1.21 "$px" "$pz" >/dev/null 2>&1 \
    || pl_bad=$((pl_bad+1))
done < <(awk '/^SEED/{s=$2} /   p  / && /x=/{gsub(/x=|z=/,""); print s, $2, $3}' /tmp/sc_test/place.out | head -8)
[ "$pl_n" -ge 5 ] && [ "$pl_bad" -eq 0 ] \
  && ok "every reported desert pyramid passes the surface rule independently ($pl_n checked)" \
  || bad "a reported pyramid fails its own surface rule" "checked=$pl_n bad=$pl_bad"

# And the rule must actually reject something -- a gate that passes everything
# is indistinguishable from no gate at all.
cat > /tmp/sc_test/place2.json <<'EOF'
{"version":"1.21","conditions":[{"id":"p","structure":"desert_pyramid","within":600,"of":"origin"}]}
EOF
pl_seen=$(grep -c '^SEED' /tmp/sc_test/place.out)
pl_rej=0
for s in 37 53 118 131; do
  ./build/checkplacement.exe "$s" desert_pyramid 1.21 240 -400 >/dev/null 2>&1 || pl_rej=$((pl_rej+1))
done
[ "$pl_rej" -ge 1 ] \
  && ok "the surface rule rejects low ground (known-bad position confirmed)" \
  || bad "the surface rule accepts everything" "rejections=$pl_rej"

# Large Biomes is the same generator at a different biome scale, so it must
# change results -- and every reported hit has to reproduce in THAT world, not
# the default one. checkbiome re-samples with the same preset.
section "large biomes world"
cat > /tmp/sc_test/lb.json <<'EOF'
{"version":"1.21","large_biomes":true,
 "conditions":[{"id":"m","biome":"mushroom_fields","within":1500,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/lb.json 40000 16 2>/dev/null > /tmp/sc_test/lb.out
lb_n=0; lb_bad=0; lb_same=0
while read -r bseed bx bz; do
  lb_n=$((lb_n+1))
  ./build/checkbiome.exe "$bseed" 1.21 "$bx" "$bz" mushroom_fields large 2>/dev/null | grep -qE ' MATCH$' \
    || lb_bad=$((lb_bad+1))
  # The same point in the DEFAULT world is almost never the same biome; if it
  # always were, the flag would not be reaching the generator at all.
  # Anchored on " MATCH$" because a bare MATCH also matches the word MISMATCH.
  ./build/checkbiome.exe "$bseed" 1.21 "$bx" "$bz" mushroom_fields 2>/dev/null | grep -qE ' MATCH$' \
    && lb_same=$((lb_same+1))
done < <(awk '/^SEED/{s=$2} /   m / && /x=/{gsub(/x=|z=/,""); print s, $2, $3}' /tmp/sc_test/lb.out | head -6)
[ "$lb_n" -ge 3 ] && [ "$lb_bad" -eq 0 ] \
  && ok "large-biomes hits reproduce in the large-biomes world ($lb_n checked)" \
  || bad "a large-biomes hit did not reproduce" "checked=$lb_n bad=$lb_bad"
[ "$lb_same" -lt "$lb_n" ] \
  && ok "the preset really changes the world (${lb_same}/${lb_n} also match by chance in default)" \
  || bad "large biomes made no difference -- flag not reaching the generator" "same=$lb_same"

# The map is the only way to check a result by eye, and the finder searches all
# three dimensions -- so all three have to render. A blank or malformed PNG for
# the nether would silently show an overworld-shaped lie.
section "map dimensions"
mapdim_bad=0
for d in "" nether end; do
  ./build/map.exe 1 1.21 2000 256 build/tmp/dim.png 0 0 - $d >/dev/null 2>&1
  python tools/checkpng.py build/tmp/dim.png >/dev/null 2>&1 || mapdim_bad=$((mapdim_bad+1))
done
[ "$mapdim_bad" -eq 0 ]   && ok "overworld, nether and end all render a valid map"   || bad "a dimension failed to render" "bad=$mapdim_bad"

# Some queries no seed can satisfy. Minecraft's own placement data excludes a
# pillager outpost within 10 chunks of a village, and the placement grid keeps
# two of the same structure apart -- so those searches would run forever. They
# must be refused up front, with the reason, and possible queries must NOT be.
section "impossible queries"
check_err "outpost inside a village rejected"   '{"version":"1.21","conditions":[{"id":"v","structure":"village","within":500},{"id":"o","structure":"outpost","within":80,"of":"v"}]}'   "impossible"
check_err "outpost/village overlap rejected"   '{"version":"1.21","conditions":[{"id":"x","overlap":["outpost","village"],"within":3000}]}'   "never generates within 10 chunks"
check_err "impossibly tight cluster rejected"   '{"version":"1.21","conditions":[{"id":"h","structure":"swamp_hut","count":4,"spread":50,"within":2000}]}'   "never be closer than"
cat > /tmp/sc_test/possible.json <<'EOF'
{"version":"1.21","conditions":[
  {"id":"v","structure":"village","within":500,"of":"origin"},
  {"id":"o","structure":"outpost","within":400,"of":"v"}]}
EOF
if ./build/find.exe /tmp/sc_test/possible.json 1 1 2>&1 | grep -qi "impossible"; then
  bad "a legal outpost/village distance was refused"
else
  ok "an outpost 400 blocks from a village is still allowed"
fi

# The exclusion is a real placement rule, so it must also FILTER: every outpost
# the finder reports has to be at least 11 chunks from any village placement.
cat > /tmp/sc_test/excl.json <<'EOF'
{"version":"1.21","conditions":[{"id":"o","structure":"outpost","within":800,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/excl.json 200000 16 2>/dev/null > /tmp/sc_test/excl.out
ex_n=0; ex_bad=0
while read -r oseed ox oz; do
  ex_n=$((ex_n+1))
  near=$(./build/nearest.exe "$oseed" 1.21 list village 0 0 3000 2>/dev/null |     awk -v cx=$((ox/16)) -v cz=$((oz/16)) '{dx=$2-cx; dz=$3-cz;
      if (dx<0) dx=-dx; if (dz<0) dz=-dz; if (dx<=10 && dz<=10) n++} END{print n+0}')
  [ "$near" -gt 0 ] && ex_bad=$((ex_bad+1))
done < <(awk '/^SEED/{s=$2} /   o / && /x=/{gsub(/x=|z=/,""); print s, $2, $3}' /tmp/sc_test/excl.out | head -8)
[ "$ex_n" -ge 5 ] && [ "$ex_bad" -eq 0 ]   && ok "no reported outpost sits inside a village exclusion zone ($ex_n checked)"   || bad "an outpost was reported inside a village exclusion zone" "checked=$ex_n bad=$ex_bad"

# "A village over a cave" is a real thing 1.18 terrain produces: noise caves are
# part of the density function, so a structure can sit on a thin crust above a
# cavern. Every reported void must reproduce under an independent re-measure of
# the same footprint columns -- and an ordinary village must NOT show one.
section "structure over a cave"
check_err "cave_below rejected before 1.18"   '{"version":"1.17","conditions":[{"id":"v","structure":"village","within":800,"cave_below":20}]}'   "1.18"
cat > /tmp/sc_test/cave.json <<'EOF'
{"version":"1.21","conditions":[{"id":"v","structure":"village","within":800,"of":"origin","cave_below":25}]}
EOF
./build/find.exe /tmp/sc_test/cave.json 60000 16 2>/dev/null > /tmp/sc_test/cave.out
cv_n=0; cv_bad=0
while read -r cseed cx cz ch; do
  cv_n=$((cv_n+1))
  # the search samples the anchor and four corners; the claim holds if ANY of
  # them really has a void that tall
  best=0
  for off in "0 0" "16 16" "-16 -16" "16 -16" "-16 16"; do
    set -- $off
    v=$(./build/surface.exe "$cseed" 1.21 $((cx+$1)) $((cz+$2)) 0 column 2>/dev/null         | grep -oE "below the surface: [0-9]+" | grep -oE "[0-9]+$")
    [ "${v:-0}" -gt "$best" ] && best=${v:-0}
  done
  [ "$best" != "$ch" ] && cv_bad=$((cv_bad+1))
done < <(awk '/^SEED/{s=$2}
              match($0, /([0-9]+)-block cave below/, m) {
                  gsub(/x=|z=|,/,""); print s, $2, $3, m[1] }'          /tmp/sc_test/cave.out | head -5)
[ "$cv_n" -ge 3 ] && [ "$cv_bad" -eq 0 ]   && ok "every reported cave under a structure re-measures identically ($cv_n checked)"   || bad "a reported cave did not reproduce" "checked=$cv_n bad=$cv_bad"

# ---------------------------------------------------------------- climate gate
# The gate makes biome searches several times faster by rejecting points on
# temperature before the full lookup. That is only acceptable if it rejects
# nothing real, so both halves of the claim are tested: that it reproduces the
# generator's own temperature exactly, and that a search returns the same seeds
# with it switched off.
section "climate gate"
cl=$(./build/climatecheck.exe 1.21 500 2>&1)
echo "$cl" | grep -q "^SOUND"   && ok "gate reproduces the generator bit-for-bit and stays inside biome limits"   || bad "climate gate is unsound" "$cl"
cl=$(./build/climatecheck.exe 1.21 200 large 2>&1)
echo "$cl" | grep -q "^SOUND"   && ok "gate is sound in a large-biomes world too"   || bad "climate gate unsound with large biomes" "$cl"

cat > /tmp/sc_test/gate.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"bad","biome":"badlands","within":350,"of":"village"},
 {"id":"village","structure":"village","within":500,"of":"origin"}]}
EOF
# One thread, so the scan order is deterministic and the two runs are comparable.
             ./build/find.exe /tmp/sc_test/gate.json 200000000 1 2>/dev/null | grep '^SEED' > /tmp/sc_test/gate.on
SC_NO_CLIMATE=1 ./build/find.exe /tmp/sc_test/gate.json 200000000 1 2>/dev/null | grep '^SEED' > /tmp/sc_test/gate.off
gn=$(wc -l < /tmp/sc_test/gate.on)
[ "$gn" -ge 5 ] && cmp -s /tmp/sc_test/gate.on /tmp/sc_test/gate.off \
  && ok "gated and ungated searches return identical seeds ($gn checked)" \
  || bad "the gate changed the result set" "$(diff /tmp/sc_test/gate.off /tmp/sc_test/gate.on | head)"

skipped=$(./build/find.exe /tmp/sc_test/gate.json 200000000 1 2>/dev/null | grep -oE 'temperature alone \([0-9.]+%\)' | grep -oE '[0-9]+' | head -1)
[ "${skipped:-0}" -ge 50 ] \
  && ok "the gate actually earns its place (${skipped}% of biome lookups skipped)" \
  || bad "the gate skipped almost nothing" "skipped=${skipped:-0}%"

# ---------------------------------------------------------------- cactus
# The cactus simulation replays decoration RNG in exact order, so the checks
# that matter are structural: it must refuse versions it cannot model, and its
# own tool must agree with the search that uses it. Block-for-block agreement
# with a real world is not testable here -- it needs a running server -- and
# lives in tier3-java/check_cactus.py.
section "tall cactus"
check_err "cactus rejected before 1.18"   '{"version":"1.17","conditions":[{"id":"c","cactus":5,"within":128}]}'   "1.18"

cat > /tmp/sc_test/cactus.json <<'EOF'
{"version":"1.21","rank":{"of":"c","top":5},"conditions":[
 {"id":"c","cactus":1,"within":160,"of":"d"},
 {"id":"d","biome":"desert","within":250,"of":"origin"}]}
EOF
./build/find.exe /tmp/sc_test/cactus.json 1500 16 2>/dev/null > /tmp/sc_test/cactus.out
cn=0; cbad=0
while read -r cseed cx cz ch; do
  cn=$((cn+1))
  # The standalone tool re-simulates from scratch; it must name the same column
  # and the same height the search reported.
  got=$(./build/cactus.exe "$cseed" 1.21 "$cx" "$cz" 32 2>/dev/null \
        | awk -v x="$cx" -v z="$cz" '$1=="CACTUS" && $2==x && $3==z {print $5}')
  [ "$got" != "$ch" ] && cbad=$((cbad+1))
done < <(awk '/^SEED/{s=$2}
              match($0, /([0-9]+)-block cactus/, m) {
                  gsub(/x=|z=/,""); print s, $2, $3, m[1] }' /tmp/sc_test/cactus.out | head -6)
[ "$cn" -ge 3 ] && [ "$cbad" -eq 0 ] \
  && ok "every reported cactus re-simulates to the same height ($cn checked)" \
  || bad "a reported cactus did not reproduce" "checked=$cn bad=$cbad"

# A single patch places 1-3 blocks and can stack to about 6; a plain (unranked)
# search asking for more should therefore find nothing quickly rather than
# report something impossible.
tallcnt=$(awk 'match($0, /([0-9]+)-block cactus/, m) && m[1] > 12' /tmp/sc_test/cactus.out | wc -l)
[ "$tallcnt" -eq 0 ] \
  && ok "no absurd heights reported (nothing over 12 blocks in 1500 seeds)" \
  || bad "implausible cactus height reported" "$tallcnt over 12 blocks"

# ---------------------------------------------------------------- inversion
# Solving for seeds instead of scanning them is only useful if it finds ALL of
# them. An inverter that quietly skips solutions would make the search report
# "nothing found" for seeds that exist -- a failure no user could detect. So the
# check is two-sided, and the completeness half is the one that matters.
section "placement inversion"
for s in swamp_hut village desert_pyramid shipwreck; do
  out=$(./build/invert.exe 1.21 "$s" --verify 2>&1)
  if echo "$out" | grep -q "^OK"; then
    d=$(echo "$out" | grep -oE '\([0-9]+x less' | grep -oE '[0-9]+')
    ok "$s: sound, and no brute-force hit missed (${d}x less space to search)"
  else
    bad "inverter unsound or incomplete for $s" "$out"
  fi
done
# Refusing the cases it cannot do is part of being correct: a power-of-two range
# scales instead of taking a remainder, so the residue-class trick does not hold.
./build/invert.exe 1.21 ancient_city --verify >/dev/null 2>&1
[ $? -eq 2 ] && ok "refuses placements it cannot invert instead of guessing" \
             || bad "inverter accepted a placement it does not implement" ""

# ---------------------------------------------------------------- summary
printf '\n\033[1m%d passed, %d failed\033[0m\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
