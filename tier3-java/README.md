# tier3-java — a real Java world as a block oracle

Every correctness claim elsewhere in this project is checked against something
independent: a JDK reference for the RNG, cubiomes for biomes and structures,
the headless 1.21.1 backend for jigsaw assembly, Bedrock Dedicated Server for
Bedrock. Java **features** — anything decoration places after terrain — had no
such source, because running them needs a `WorldGenLevel` that the headless
backend cannot build.

So this runs the actual server and reads blocks out of the world it generates.

## Setup

`server.jar` is fetched from Mojang's official metadata (never committed —
it is 51 MB and not ours to redistribute), and its SHA-1 is checked against the
manifest:

```
python -c "
import hashlib,json,urllib.request,os
m=json.load(urllib.request.urlopen('https://launchermeta.mojang.com/mc/game/version_manifest_v2.json'))
v=next(x for x in m['versions'] if x['id']=='1.21.1')
s=json.load(urllib.request.urlopen(v['url']))['downloads']['server']
urllib.request.urlretrieve(s['url'],'tier3-java/server.jar')
h=hashlib.sha1(open('tier3-java/server.jar','rb').read()).hexdigest()
print('sha1',h,'MATCH' if h==s['sha1'] else 'MISMATCH')"
```

Running it writes `eula=true`, which accepts Mojang's EULA on this machine.
That was done deliberately and with permission, not silently.

## Use

```python
from java_oracle import JavaServer
with JavaServer(seed=12345) as srv:
    srv.forceload(x, z)
    print(srv.cactus_height(x, z))
```

One server, ~8 s to boot, ~40 ms per probe batch. Console commands all run in
the same tick, so `run([...])` with a thousand commands costs about what one
costs — batch aggressively.

`run-<seed>/` worlds are gitignored and can be deleted freely.

## The trap this is built around

`/execute if block` on a chunk that is not loaded fails **exactly** like a block
that is not there. Merging those two is how the Bedrock work reported "no
structure here" three separate times for chunks it had simply never loaded.

So every probe is preceded by a positive control: y=−64 is bedrock in every
overworld column, so if that fails, the chunk is not loaded and the answer is
`None` — unknown — never `False`.

## check_terrain.py — what it found

Diffs `src/terrain.c` against the real world. Getting the *comparison* right
took three attempts, and each wrong version accused correct code:

| Framing | Verdict | Reality |
|---|---|---|
| "highest non-air block must equal our surface" | terrain broken at (1190,1290) | an **iceberg**: packed ice on y=66 over a seabed at 46, and icebergs are a feature |
| ice counted as terrain | broken at (1676,916) and (947,-523) | a frozen **ice sheet** at 62 over gravel at 61 |
| snow_block *not* counted as terrain | broken at (461,-149) | our y=165 **is** a snow_block; the `snow` layer at 166 is the decoration |

The check that survives is narrower and tests our claim directly rather than
inferring it: the block at our reported height must be a terrain material, and
the block one above must not be. What decoration stacked on top is then
irrelevant, which is the whole point.

**A real limitation it did surface.** Under water our heights are wrong, badly:
at (1595,1645) we report solid ground at y=57 where the true seabed is y=33, a
24-block error. That is 1.18+ **aquifers** — the generator carves water out of
what the base noise called solid, and cubiomes' `generateColumn` does not model
them. Submerged columns are therefore reported separately, and any height,
relief or `cave_below` result below sea level should be treated as unverified.
