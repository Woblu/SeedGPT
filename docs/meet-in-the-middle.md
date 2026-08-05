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
