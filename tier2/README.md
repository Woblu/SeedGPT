# Tier-2 worldgen backend (village buildings)

cubiomes can't do terrain-dependent generation, so it cannot enumerate the
buildings inside a 1.14+ jigsaw village. Neither can any seedfinding library:
FeatureUtils / mc_feature both ship an *unfinished* village generator (the
jigsaw assembler was never completed). The only correct way to count a
village's buildings is to run Minecraft's **actual** world generator.

This backend does exactly that, headlessly, via Fabric Loom (which downloads a
deobfuscated Minecraft and puts it on the classpath). `VillageWorldgen`:

1. `Bootstrap.bootStrap()` + a vanilla data-pack `ResourceManager` (so village
   templates load) + `StructureManager`.
2. builds the overworld biome source + noise chunk generator for a seed,
3. finds village start chunks near a radius, generates each village with the
   real jigsaw assembler, and
4. counts smith buildings by template name (`*_tool_smith`, `*_weapon_smith`,
   `*_armorer`), printing `seed  x  z  biome  smiths=N  {breakdown}`.

Proven: seed 88 @ (-248,1144) is a taiga village with **5 smiths**
(1 armorer, 1 toolsmith, 3 weaponsmith).

## Run

```sh
# from tier2/, with a Gradle distribution (see ../tools/gradle-dist):
../tools/gradle-dist/gradle-8.5/bin/gradle --no-daemon printcp -q | sed 's/^CP://' | tr '\n' ';' > cp.txt
javac -cp "$(cat cp.txt)" -d out src/main/java/VillageWorldgen.java
java -cp "out;$(cat cp.txt)" VillageWorldgen <seed> <radius> <minSmiths>
```

## Status / caveats

- **POC.** Targets MC **1.16.5** (data-driven worldgen; 1.16.1 predates it and
  needs the older hardcoded API). Village *composition* logic is identical
  across 1.16.x; village *positions* differ from 1.16.1 (legacy biome init).
- Not yet wired to the C finder. The intended two-tier flow: cubiomes finds
  villages fast → this backend counts buildings on the survivors → filter.
- Adds a Minecraft dependency (downloaded per build by Loom; not bundled).
