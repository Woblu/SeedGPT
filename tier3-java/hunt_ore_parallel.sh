#!/usr/bin/env bash
# Shard the ore-casing hunt across N workers on disjoint seed ranges.
#
# One worker spends nearly all its wall clock waiting on chunk generation in a
# single server, so on a 16-core box it leaves most of the machine idle. Six
# workers is the memory limit rather than the CPU one -- each server wants ~2 GB
# against 32 GB total, and worlds are ~60 MB each and deleted as they are read.
#
#   ./hunt_ore_parallel.sh [workers] [seeds-per-worker] [radius] [start]
#
# Each worker writes its own log and jsonl so nothing interleaves; read them with
#   tail -n2 ore_hunt_*.log        # progress
#   grep -h "best so far" ore_hunt_*.log | sort -t' ' -k4 -rn | head
set -u
W=${1:-6}
PER=${2:-25}
RADIUS=${3:-4000}
START=${4:-1000}

cd "$(dirname "$0")" || exit 1
rm -f ore_hunt_*.log ore_finds_*.jsonl

for i in $(seq 0 $((W - 1))); do
    s=$((START + i * PER))
    nohup python hunt_treasure_ore.py --target any --min 1 \
        --seeds "$PER" --start "$s" --radius "$RADIUS" \
        --json "ore_finds_$i.jsonl" > "ore_hunt_$i.log" 2>&1 &
    echo "worker $i: seeds $s..$((s + PER - 1))  -> ore_hunt_$i.log"
done
echo "$W workers, $((W * PER)) seeds, radius $RADIUS"
