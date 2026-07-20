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
&& ./build.sh tools/describe.c >/tmp/sc_test/b4 2>&1; then
  ok "all tools compile"
else
  bad "build failed" "$(cat /tmp/sc_test/b1 /tmp/sc_test/b2 /tmp/sc_test/b3 /tmp/sc_test/b4 2>/dev/null | grep -i error | head -3)"
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
 {"id":"mansion","structure":"mansion","within":500,"of":"spawn"}]}
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
 {"id":"village","structure":"village","within":1000,"of":"spawn"},
 {"id":"mansion","structure":"mansion","within":1000,"of":"spawn"}]}
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
  '{"version":"1.21","conditions":[{"id":"f","structure":"fortress","within":300,"of":"spawn"},{"id":"v","structure":"village","within":500,"of":"f"}]}' \
  "cross-dimension"
check_err "bad biome precision rejected" \
  '{"version":"1.21","conditions":[{"id":"j","biome":"jungle","within":300,"precision":"turbo"}]}' \
  "fast|fine|exact"

# Biome scan precision must change the plan's cost estimate, not just parse.
cat > /tmp/sc_test/prec.json <<'EOF'
{"version":"1.21","conditions":[{"id":"j","biome":"jungle","within":400,"of":"spawn"}]}
EOF
cfine=$(./build/find.exe /tmp/sc_test/prec.json 1 1 2>&1 | sed -n 's/.*jungle.*\[fine\] *~\([0-9]*\) ns.*/\1/p' | head -1)
sed -i 's/"of":"spawn"}/"of":"spawn","precision":"fast"}/' /tmp/sc_test/prec.json
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
 {"id":"v1","structure":"village","within":400,"of":"spawn"},
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

# ---------------------------------------------------------------- dimensions
section "nether / end"
cat > /tmp/sc_test/nether.json <<'EOF'
{"version":"1.21","conditions":[
 {"id":"fortress","structure":"fortress","within":300,"of":"spawn"},
 {"id":"bastion","structure":"bastion","within":500,"of":"fortress"}]}
EOF
./build/find.exe /tmp/sc_test/nether.json 2000000 16 2>&1 | grep -q '^SEED' \
  && ok "nether query (fortress + bastion) returns seeds" \
  || bad "nether query found nothing"

cat > /tmp/sc_test/end.json <<'EOF'
{"version":"1.21","conditions":[{"id":"city","structure":"end_city","within":3000,"of":"spawn"}]}
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

# ---------------------------------------------------------------- summary
printf '\n\033[1m%d passed, %d failed\033[0m\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
