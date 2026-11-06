# Meet-in-the-middle for structure placement

The derivation, the one obstacle that is not obvious, and how to prove the
result. Written before implementing so the build is arithmetic rather than
discovery — the same reason `docs/cactus-generation.md` exists.

## What it is for

`tools/quad.c` solves **two** of the eight constraints in a quad-hut search
(the first structure's x and z) and tests the other six. That is ~140× better
than brute force and still ~9 hours for a complete enumeration at spread 10.

Meet-in-the-middle solves all of them at once: build a table over the low half
of the seed, then sweep the high half against it. 2⁴⁸ becomes ~2²⁴ + 2²⁴.
Hours become seconds.

## Setup

For one structure in one region, with `off = regX·341873128712 +
regZ·132897987541 + salt`:

```
s0 = (ws + off) ^ K                    K = 0x5deece66d, b = 0xb, M = 2⁴⁸−1
s1 = (s0·K + b) & M        ox = (s1 >> 17) % r
s2 = (s1·K + b) & M        oz = (s2 >> 17) % r
```

## The split

Write `ws = wsH·2²⁴ + wsL`. Multiplication mod 2⁴⁸ has the property that the
low *n* bits of a product depend only on the low *n* bits of its operands, and
XOR is bitwise, so the halves separate:

```
lowsum = (wsL + offL) mod 2²⁴          c = 1 if wsL + offL ≥ 2²⁴ else 0
s0L    = lowsum ^ KL
s0H    = ((wsH + offH + c) mod 2²⁴) ^ KH

P   = s0L·K + b
s1L = P mod 2²⁴                        ← depends on wsL ONLY
s1H = (s0H·K + (P >> 24)) mod 2²⁴      ← depends on wsH, and on wsL via c and P>>24
```

`c` is the only thing the low half hands upward besides `P >> 24`. That is the
whole reason the split works.

## The constraint, and the free 8× filter

`s1 >> 17` spans bits 17..47, which is `s1H` shifted up by 7 plus the top 7
bits of `s1L`:

```
s1 >> 17 = s1H·128 + (s1L >> 17)
```

so the requirement `(s1 >> 17) ≡ ox (mod r)` with r = 24, and 128 mod 24 = 8:

```
8·s1H + (s1L >> 17) ≡ ox   (mod 24)
```

Let `d = (ox − (s1L >> 17)) mod 24`. Since **gcd(8, 24) = 8**:

- if `8 ∤ d` — **no wsH whatsoever can satisfy this**. Reject this `wsL`
  outright, having touched none of the high half. That discards 7 of every 8
  low halves per constraint, for free.
- otherwise `s1H ≡ d/8  (mod 3)`.

This is the gist's "signature … required offsets mod 25" — mod 25 there because
ruined portals have range 25; mod 3 here because 24 = 8·3.

## The obstacle (do not skip this)

`s1H` is reduced **mod 2²⁴**, and `2²⁴ ≡ 1 (mod 3)`. So for `x = s0H·K + (P>>24)`:

```
(x mod 2²⁴) ≡ x − k   (mod 3),   k = ⌊x / 2²⁴⌋
```

`s0H < 2²⁴` and `K ≈ 2³⁴·⁵`, so `k` ranges over ~2³⁴ values. **The mod-3
condition does not factor cleanly through the truncation** — you cannot just
store `s1H mod 3` in the table and match it.

This is exactly why the gist tracks *carry patterns* rather than residues
alone. Two ways out, and the first is likely simpler:

1. **Carry the quotient.** The table entry records what the low half
   contributes (`c`, `P>>24`, and the required `d/8`); the high sweep computes
   `x = s0H·K + (P>>24)` in full 64-bit, takes `mod 2²⁴`, then `mod 3`
   directly. No factoring needed — the reduction is done concretely per
   candidate. This keeps the table small and moves the work into the sweep,
   which is fine because the sweep is the cheap half.
2. **Enumerate k's residue.** Split the sweep by `k mod 3` and fold it into
   the signature. More table, less arithmetic per candidate.

Start with (1). It is obviously correct; (2) is an optimisation to measure
against it, not to assume.

## The join that makes it a real MITM

(1) above removes the obstacle but leaves a nested loop. The hash join is
available too, and this is the piece worth having.

Split on `y = ws + off_0` rather than on `ws`, so every structure's pre-XOR
seed is `y + D_j` for a known constant `D_j = off_j − off_0`. Then

```
s0_jH = ((yH + D_jH + c_j) mod 2²⁴) ^ KH
```

— every high-half input is `yH` plus a **known** constant, and `c_j ∈ {0,1}`,
so across `n` structures there are only `2ⁿ` carry patterns. Precompute once:

```
T[v] = ((v ^ KH)·K) mod 2²⁴          v over 2²⁴, ~64 MB as uint32
s1_jH = (T[yH + shift_j] + PH_j) mod 2²⁴     shift_j = D_jH + c_j
```

Now build, per carry pattern, a bucket index over all `2²⁴` values of `yH`
keyed by the tuple `(T[yH + shift_j] mod 3)` for j = 1..n — that is `3ⁿ`
buckets, 81 for a quad. The low half computes the tuple it needs,
`((e_j − PH_j) mod 3)_j`, and **looks it up** instead of sweeping. Meet in the
middle, properly.

The mod-3 truncation problem does not arise here: `T` is built by doing the
`mod 2²⁴` concretely, so nothing is ever factored through it.

## Sizing it, so the win is known before the work

Two filters compound, and the first is nearly free:

- `8 | d_j` is computable from `yL` alone and kills **7 of 8** per constraint.
  Four structures constrained on x leaves ~`2²⁴/4096 = 4096` surviving low
  halves. (In a quad hunt the corner offset is not a single value but a small
  set, so the real rate is per allowed offset — measure it, do not assume 1/8.)
- The bucket lookup then replaces a `2²⁴` sweep per survivor with one bucket.

Even the *nested* version — every surviving `yL` against all `2²⁴` `yH`, no
join — is ~`4096 × 2²⁴ ≈ 7×10¹⁰` concrete tests, which at a few hundred
million per second across threads is minutes rather than `quad.c`'s ~9 hours.
That is worth building first precisely because it needs no clever reasoning:
if the join version does not agree with it exactly, the join version is wrong.

So the build order is: nested (obviously correct, minutes) → join (fast) →
prove they return the identical set.

## Second axis and further structures

`oz` uses `s2 = s1·K + b`, which is the same shape one step along, so the same
split applies with `s1` in place of `s0`. Additional structures reuse the
identical machinery with a different `off` — and crucially the **same** `wsL`
and `wsH`, which is what makes eight constraints collapse together instead of
multiplying.

## How to know it works

Do not invent a new harness. `tools/invert.c --verify` already brute-forces 4
million world seeds, keeps every one that lands on the target, and demands the
enumeration would have reached all of them — it currently reports **0 missed**
across four structure types. Point that same completeness test at the MITM
enumerator.

The failure mode is silent: an MITM that drops seeds reports "no quad huts" for
configurations that exist, and nothing about the output looks wrong. So the
completeness half is not optional, and a green soundness check alone means
nothing here.

Cross-check the finished thing against `tools/quad.c` on a loose spread where
bases are common (spread 16 finds them in seconds): both must produce the
**same set**, not merely similar counts.

## Every region-based structure, not just the easy ones

The first cut handled ten structures and refused eight. Reading the engine
rather than the refusal shows most of those eight were rejected for reasons that
do not hold:

| structure | why it was refused | what is actually true |
|---|---|---|
| ancient city | power-of-two range | `ox = (r·bits)>>31` with r = 2ᵏ is **exactly** `s1H >> (24−k)`. The low half contributes at most 127 to a value shifted right by 24−k+7, so it can never carry. A *prefix* of the high half instead of a residue — cleaner than the modular case, not harder. |
| fortress (1.18+) | "different draw pattern" | plain `getFeaturePos`, and `getStructurePos` returns 1 unconditionally. Nothing to handle at all. |
| bastion (1.18+), outpost | "adds a rejection roll" | the roll is a function of the 48-bit seed and the chunk, so it is one more LCG walk per candidate and costs the sieve nothing. |
| monument, mansion, end city | "two draws averaged" | four draws instead of two. The pair constraint couples them, so the sieve uses each draw's marginal — sound, weaker. End city's 1008-block gap is one more concrete test. |

What is genuinely out of reach is what uses a *different algorithm*: mineshafts
and buried treasure are per-chunk rolls, strongholds are ring-based, and the
decorator features (geodes, wells) run on Xoroshiro population seeds. Those are
not this problem with a twist; they are another problem.

Two things the extension taught, both the hard way:

- **The averaged offset is triangular, not uniform.** `(d1+d2)>>1` concentrates
  in the middle: for a mansion, offset 59 needs both draws to be 59, one chance
  in 3600. So a mansion 4-cluster does not exist at any tight spread, and the
  harness demanding one was wrong about the solver rather than the reverse.
- **Modelling rarity to decide whether a test is vacuous is a losing game.** It
  was wrong three times running — the rejection rolls, then the triangular
  distribution, then whatever correlates neighbouring outposts' rolls. The
  harness now reports what it measured: if the independent reference found
  nothing in the sample either, there was nothing to miss, and that is all it
  claims.

**A large range with averaged draws is the algorithm's worst case, and mansions
are it.** The pair constraint means the sieve can only use each draw's marginal,
and for a single target offset that marginal spans most of the 60 possible
values — so the sieve keeps every low half. The bucket key is then the only
filter, and with `m = 15` and offsets that wide it does not filter either. Both
halves idle at once, which is the opposite of the swamp-hut and ruined-portal
cases where exactly one of them carries the search.

That is a property, not a bug. But it was also, for a long time, the wrong
diagnosis of why mansion's `--verify` ran for forty minutes. Three fixes were
guessed and applied — threading the heaviest check, budgeting in tests instead
of seeds, measuring the sweep rate instead of modelling it — and each helped
without solving it, because none of them was the actual problem.

Timestamping every line found it in one run:

```
18:12  1 structure, nested   ->  18:23  1 structure, join   = 11 minutes
18:23  2 structures, nested  ->  18:34  2 structures, join  = 11 minutes
```

The *nested* sweeps were instant; the **joins** took eleven minutes each — on a
windowed run where `yhN` is one or two high halves. `joinWorker` was iterating
all 2²⁴ surviving low halves and calling `classesFor` on every one, which loops
octants × residues — 3840 iterations for a mansion. Six times 10¹⁰ operations of
setup, to avoid sweeping two values. The guard that would have caught it ran
*after* paying that cost.

A sweep of `yhN` can never lose to a walk costing `n·C` just to set up, so that
comparison now happens first. **Mansion's whole verify went from >40 minutes to
2m29s, end city to 2m01s**, and both pass.

Two lessons worth keeping, both about method rather than arithmetic:

- **Measure before fixing.** Every one of the three earlier fixes was a
  plausible story about where the time went, and all three were wrong. One run
  with timestamps beat all the reasoning.
- **A guard that runs after the expensive part is not a guard.** The join
  already knew a sweep would be cheaper; it just worked that out too late.

The other two changes are kept because they are real improvements in their own
right: the threaded rejection sweep (swamp hut's `--verify`: ~2 minutes to
**25 seconds**) and the measured window sizing.

## What was built, and where the plan was wrong

`src/mitm.c` + `tools/mitm.c`. Both halves exist and agree. Three things the
plan above did not have right, kept here because the corrections are the
interesting part:

**The join's key was unsound as written.** The plan says to bucket `yH` by
`(T[yH+shift_j] mod 3)` and look it up with `((e_j − PH_j) mod 3)`. That drops
seeds. `s1_jH = (T[…] + PH_j) mod 2²⁴`, and the truncation is exactly the
obstacle the plan had already identified two sections earlier — it does not go
away by moving to a table, because `PH_j` is added *after* the table lookup.
The addition wraps when `T ≥ 2²⁴ − PH_j`, a threshold that moves with the low
half, and the residue the low half needs differs on either side of it. The fix
is to put the **octant of `T`** in the key as well: the threshold then lies in
one octant, seven have a known carry (one residue each) and the straddling one
takes two. That is 9 of 24 classes per structure rather than 3 of 3 — a smaller
cut than the unsound version promised, and an actual one. The nested-vs-join
equality test is what caught it; nothing about the wrong version looked wrong.

**The `8 | d` filter is not a free 8× per constraint.** Its strength is
`|allowed offsets| / 8` per constraint, so it is only an 8× when the offset is
pinned exactly. At spread 16 the allowed set is 8 consecutive offsets, exactly
one of which is a multiple of 8 — so the sieve keeps *everything* and does no
work at all.

**The z axis sieves too, and that is where the win comes from.** `s2L` depends
only on `s1L`, so `oz` gives an identical free filter on the low half. Eight
constraints, not four. With the offsets pinned that is `2²⁴/8⁸ ≈ 1` surviving
low half — the low 24 bits of the seed are determined outright.

**Everything scales with `gcd(128, range)`, in opposite directions for the two
halves.** The sieve pins the high half mod `m = range/gcd(128, range)`, so:

| structure | range | gcd | m | sieve keeps (8 exact) | join vs sweep, measured |
|---|---|---|---|---|---|
| swamp hut, desert pyramid | 24 | 8 | 3 | 16 of 2²⁴ | 70× fewer pairs |
| shipwreck | 20 | 4 | 5 | — | — |
| village | 26 | 2 | 13 | 112 000 of 2²⁴ | 5 155× fewer pairs |
| ruined portal | 25 | 1 | 25 | **all** of 2²⁴ | **27 966×** fewer pairs |

(`--verify` is green for all five invertible families — swamp hut, desert
pyramid, shipwreck, village, ruined portal — which is the point of covering the
full range of `gcd` rather than the one structure the quad hunt cares about.)

A big `gcd` makes the low-half sieve strong and the bucket key weak — only 3
residues to key on. A small one does the reverse. At the extreme, ruined portals
have `gcd(128, 25) = 1`, so `8 | d` is satisfied by every `d`: the sieve rejects
**nothing**, every low half is live, and the join is doing all the work. (That
is also why the gist this came from talks about "required offsets mod 25" — it
was written for ruined portals, the one case where the low-half filter is
free of charge and worth exactly nothing.)

So the two filters are not a compounding pair — they are strong in opposite
regimes, and `gcd(128, range)` decides which one carries the search. Building
only the sieve would have left ruined portals with no improvement whatsoever;
building only the join would have cost the swamp-hut case the sieve's own
factor of 2²⁴/16 ≈ 10⁶. Both are load-bearing, for different structures.

`--verify` reports this rather than assuming it: when the sieve rejects nothing,
the "sweep every high half against a rejected low half" check has nothing to
falsify and says so, instead of hunting forever for a rejection that cannot
happen.

Measured, swamp huts, whole 2⁴⁸ space:

| spread | low halves kept | pairs tested (nested → join) | wall clock | quad bases |
|--------|-----------------|------------------------------|------------|-----------|
| 13     | 0               | —                            | 1.2 s      | 0         |
| 14     | 64              | 1.07e9 → 2.3e7               | 1.0 s      | 0         |
| 15     | 49 712          | 8.34e11 → 1.54e10            | 318 s → 44 s | 1 045 607 |

`quad.c` covers the same ground by walking `2⁴⁸/24²` seeds **per corner offset**,
and the number of corner offsets grows as the spread loosens: 4 at spread 10,
49 at spread 15, 64 at spread 16. At its measured 1.35e8 seeds/s that is ~49
hours for the spread-15 enumeration the join does in 44 seconds. Note the shape
of it — `quad.c` gets *slower* as the target loosens because it must solve more
corner offsets, while the MITM gets slower for the opposite reason (more low
halves survive). They are not the same curve.

**Spread 13 and 14 are empty, and that is a real answer.** Under `quad.c`'s
pairwise rule the two diagonal structures are at least 9 chunks apart on *both*
axes, so no cluster is tighter than 9√2 ≈ 12.73 — and the offsets that reach
that bound turn out to have no seed at all. The tightest swamp-hut quad that
exists is **spread 15**. `quad.c`'s own docstring picks spread 10 as its worked
example — an hours-long enumeration whose complete and correct answer is
nothing, which nobody would have found out by running it.

An empty answer out of a sieve is indistinguishable from a broken sieve, so
`mitm quad` corroborates one: it solves for three of the four structures and
tests the fourth with cubiomes, a path that never asks the sieve about it. At
spread 13, 650 236 seeds place three huts exactly right and not one places the
fourth.

## Wiring it into the search

`src/solve.c` lets a normal query use the solver. When a query asks for a tight
cluster, `find` stops walking seeds 0,1,2,… and instead walks a list of seeds
that already place the cluster. `tools/find.c` changes by one line in the hot
loop — where `s48` comes from — and **nothing downstream changes**: pass 1 still
evaluates every candidate, so the solver decides what is *tried*, never what is
a *hit*. A wrong proposal is rejected exactly like any other seed.

That leaves one thing to prove: the list must not MISS clusters the scan would
find. Three things make that argument, in order of how much they are load-
bearing:

1. **The corner is the only shape.** Four structures within `spread` of a common
   member, one per region, must occupy a 2×2 region block — two regions apart on
   an axis is at least `2·regionSize − (range−1)` chunks, and under the anchored
   rule two members can be `2·spread` apart. `mitmCornerIsOnlyShape` checks that
   inequality and **declines** when it fails, so a loose spread scans as before.
2. **The rule is a disjunction, and is solved as one.** "Some member has the
   other three close" is four questions, not one. Merging their offset sets
   first makes the query so loose the sieve rejects nothing — measurably: the
   merged form took 52 s to build 300 000 candidates, the four separate ones
   take 1.0 s. Correctness and speed wanted the same thing here.
3. **Measured, not argued.** `mitm --covers <spread> <N>` brute-forces N
   structure seeds for real clusters and demands the solver would have proposed
   every one. At spread 240: 185 clusters found, 0 missed.

Solving is not always right. A loose cluster is common enough that the plain
scan finds it before the solver can enumerate, because a loose target leaves the
sieve nothing to reject. Predicting which case a query is in needs the true
cluster rate — not the relaxed bound the solver has — so `find` measures it:
build under a time budget, and fall back to scanning if the yield is poor. It
says which it did and why.

The other outcome is worth more than the speed. When the solver **completes**
without finding anything, that is not "nothing yet" — it is a proof that the
whole 2⁴⁸ space contains none, delivered in seconds where the scan would have
run until the user gave up. The README's own flagship example turns out to be
one of these: four swamp huts within **160** blocks of a common member is
impossible, because the two diagonal members cannot be closer than 9√2 chunks =
203.6 blocks. A scan reports that as 0.0000% survival, which reads as "rare".

## Nether fortresses: placement is not existence, and distance is not overlap

Two things made the quad-fortress search disappointing, and only one of them was
a tuning problem.

**The bug.** In 1.18+ a fortress and a bastion share a grid slot — identical
salt, region size and range (`30084232, 27, 23`) — and only one of them is
built: a fortress generates where the bastion does not. But
`getStructurePos(Fortress, ...)` returns 1 **unconditionally** for 1.18+, so a
search built on placement accepts every slot. The first "quad fortress" it
reported had **one** fortress and three bastions.

`mitm quad` now checks existence rather than placement for these two types, via
`isViableStructurePos`, which resolves the tie-break properly. It is only
reached by candidates that already passed the geometry, so the generator cost is
irrelevant. Placement-is-existence remains true for every other structure and is
still the fast path.

**The wrong measure.** Asking for four fortresses "close together" ranks by the
distance between their START positions, and that is not what makes fortresses
interesting. A fortress is up to 257 pieces sprawling as far as 112 blocks from
its start — and each is generated **without knowledge of the others**.
`getFortressPieces` rejects a piece colliding with an earlier piece of the *same*
fortress; it has no idea a neighbouring fortress exists. Two fortresses whose
starts are 80 blocks apart therefore do not sit near each other, they grow
*through* each other.

Start distance only bounds the opportunity. `tools/fortoverlap.c` measures what
actually happened: build all four piece lists and count the pairs whose bounding
boxes intersect.

```sh
./build/mitm.exe 1.21 fortress quad 8 --limit 6000 | grep QUADBASE | awk '{print $2}' \
  | ./build/fortoverlap.exe 0 1.21 --stdin | sort -rn | head
```

The solver proposes seeds whose four slots are tight; the overlap scores what
the pieces did with that chance.

Measured, all four fortresses verified:

| spread (chunks) | best score | shared blocks | seed |
|---|---|---|---|
| **8** | **286** | **27 592** | 272750727216669 |
| 10 | 201 | 8 540 | 8248588826346 |
| 12 | 148 | 5 476 | 117035865797502 |

Two things that only measuring shows. Across spread bands, tighter starts really
do mean deeper overlap — the loosest band is half the score of the tightest. But
*within* one band the correlation is weak: the twelve spread-8 bases range from
138 to 286, so the seed the solver happens to emit first is not the one worth
having.

And spread 8 is exhaustive: two runs with different limits returned the **same
twelve** bases, so that is the complete set at the tightest geometry the grid
allows. Wanting more of them means accepting a looser spread, and the table says
what that costs.
