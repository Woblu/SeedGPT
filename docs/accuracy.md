# How accurate is Seedsmith?

A seed finder that returns a wrong answer looks exactly like one that returns a right
answer, until you fly to the coordinates. So this page says what has been checked,
how, and what has not.

## If something isn't where it should be

Work through these in order. They explain nearly every report.

1. **Does the version match?** A seed makes a different world on a different version.
   Create the world on exactly the version shown above your results.
2. **Is it Large Biomes?** A Large Biomes search only applies to a Large Biomes world.
3. **Is it a ruined portal?** About half of those in plains and mountains generate
   underground. Tick **surface only** on the condition in **Build**, or look at what
   **describe** says about it. Seedsmith also cannot fully check whether a ruined portal generates at all, and
   it warns you when your search includes one.
4. **Was the distance measured from spawn?** Structure positions are exact, but world
   spawn is an estimate: Minecraft picks it using grass, which is decoration that
   Seedsmith does not simulate. Use the structure's coordinates, not its distance.
5. **Is it a biome edge?** A search that finds a biome samples points across an area
   and can miss small patches, so it can *miss* seeds. It does not invent them.

If it still doesn't add up, please
[report it](https://github.com/Woblu/Seedsmith/issues/new/choose). A wrong result is
the most useful bug report there is.

## What has been checked

"Engine" means it was only cross-checked within cubiomes itself. "Real" means it was
compared with Minecraft's own generator or a running server. The version shown is
where the check was made; others are expected to follow the same code, but were not run.

| Feature | Checked against | Versions |
|---|---|---|
| Structure positions, 9 types | Independent Java reference, 3,881 of 3,881 | 1.18 settings through 26.2 |
| Pyramid, jungle temple and mansion terrain rules | Real generator, 110 positions, no false positives | 1.21.1 |
| Chest loot from 1.18 on | Real server, chest by chest, 10 of 10: desert pyramids, outposts, ocean and beached shipwrecks | 1.21.1 |
| Ruined portal chests | Independent seedfinding libraries | 1.17.1 |
| Village buildings (advanced tool) | Real generator | 1.16.5 |
| Buried treasure exposure and casing | Real generator and server | 1.16.5, 1.21.1 |
| Stronghold heights | Real server, 15 of 15 below y=52 | 1.16.5 |
| Biome conditions, 1.17 and earlier | Engine only | All |
| Chest loot, other structures and versions | Engine only | All |

## Limits worth knowing

- **Seedsmith can miss seeds, but it should not invent them.** Biome sampling, and the
  number of variations tried per seed, trade completeness for speed. For a rare biome,
  raise the number of variations tried. See the [manual](reference.md#known-limitations).
- **Spawn is an estimate.** See above.
- **Some ruined portals may not exist.** Their position is exact, but whether the game
  places one depends on terrain that Seedsmith does not model.
- **Structure overlap is an approximation.** A hit means the structures' rough
  footprints meet. It is a strong candidate, not proof.
- **Bastion chest counts are a minimum.** Only the bastion's first piece is simulated.
- **Terrain height is an estimate** unless you ask for exact terrain, which is slower.
- **Windows, Linux and macOS generate identical results.** `tests/golden.py` compares
  searches, seed descriptions and rendered maps byte for byte on every change.

The [manual](reference.md#what-has-been-checked-and-against-what) has the detail
behind each of these.
