import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.StructureManager;
import net.minecraft.world.level.WorldGenLevel;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.structure.Structure;
import net.minecraft.world.level.levelgen.structure.StructureStart;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

// Ore decoration, headless.
//
// FIXED 2026-08-14, after this class spent months reporting "no ore" everywhere.
//
// Symptom: applyBiomeDecoration contributed about ten blocks per chunk -- glow
// lichen, springs -- and zero ore, zero andesite/granite/diorite, zero
// vegetation. An 8057-chest census scored 48342 casing faces and found ore on
// none of them, which measured this bug rather than Minecraft.
//
// Cause: block TAGS were never bound. RegistryDataLoader loads registry
// contents, not tags, and nothing else bound them, so
// BuiltInRegistries.BLOCK.getTag(STONE_ORE_REPLACEABLES) was absent outright and
// state.is(anyTag) was false for every block in the world. Every ore's RuleTest
// is a TagMatchTest, so each one rejected the stone it stood in and wrote
// nothing. Only features configured with inline block lists -- springs, glow
// lichen -- survived, which is exactly the signature that was measured.
//
// Fix: OutpostWorldgen.bindAllTags, called from bootstrapBody.
//
// Diagnostics kept, because each one answers a question the others cannot:
//   OreGen tags                        are tags bound at all
//   OreGen orescan <seed> <x> <z>      chunk diff with vs without decoration
//   OreGen orefeature <seed> <x> <z> <ids>
//                                      run named placed features alone, counting
//                                      positions through each placement modifier
//
// The general lesson, which this project keeps paying for: a subsystem that is
// switched off answers every question with "nothing here", and that is a valid-
// looking answer to every question anyone thought to ask. The chunk diff is what
// broke it open, because it compares against a control instead of asking the
// broken thing to describe itself.
//
// The tier-2 path generates terrain and surface rules but stops before
// applyBiomeDecoration, which is where ore is placed -- so it can say where a
// buried treasure chest lands but not what it lands ON. That gap is the reason
// the ore hunt had to fall back on the server at 6.5s per chest, against 0.09s
// here.
//
// Decoration needs a WorldGenLevel, and vanilla only ever builds one on top of a
// ServerLevel (WorldGenRegion). WorldGenLevel is an interface, so this supplies
// one by dynamic proxy: the handful of methods a feature actually calls are
// implemented against a single ProtoChunk, and every default method is delegated
// back to the interface's own implementation via invokeDefault. That keeps this
// class to the methods that matter instead of the ~80 the type nominally has.
//
// ONLY THE CHUNK ITSELF IS DECORATED, not the 3x3 neighbourhood vanilla writes
// across. That is sound HERE and nowhere else: buried treasure sits at
// (chunkX*16+9, chunkZ*16+9), the chunk's centre, about 8 blocks from every
// border, while an ore blob reaches roughly 4. So a blob originating in a
// neighbouring chunk cannot cover the chest's own position, which is the only
// position this asks about. Reading any position near a chunk border through
// this class would silently miss ore, and unimplemented() below exists so that
// a feature reaching for something we cannot supply is recorded rather than
// quietly answered with a default.
public class OreGen {

    static final Set<String> UNIMPLEMENTED = new java.util.TreeSet<>();

    /** Non-null to record every proxy method a feature invokes. */
    static TreeMap<String, Integer> TRACE = null;

    /** A WorldGenLevel backed by one ProtoChunk. */
    static WorldGenLevel levelFor(long seed, ChunkAccess chunk,
                                  NoiseBasedChunkGenerator gen, RandomState rs,
                                  BiomeManager biomes, Registry<Biome> biomeReg) {
        return levelFor(seed, chunk, gen, rs, biomes, biomeReg, null);
    }

    /** As above, but backed by a REAL neighbourhood instead of empty stand-ins.
     *
     *  `hood` maps chunk key -> generated chunk. Null keeps the old
     *  single-chunk behaviour: neighbours read as air, writes to them are
     *  dropped. Supplied, reads and writes route to the actual neighbouring
     *  chunk -- which is what vanilla does, since decoration writes across
     *  chunk borders and a blob originating next door can reach into this one.
     */
    static WorldGenLevel levelFor(long seed, ChunkAccess chunk,
                                  NoiseBasedChunkGenerator gen, RandomState rs,
                                  BiomeManager biomes, Registry<Biome> biomeReg,
                                  Map<Long, ChunkAccess> hood) {
        ChunkPos cp = chunk.getPos();
        int minX = cp.getMinBlockX(), minZ = cp.getMinBlockZ();

        InvocationHandler h = (proxy, method, args) -> {
            String n = method.getName();
            // TRACE exists because "place() returned false" names nothing. The
            // proxy is the only surface between the feature and the world, so
            // the call log IS the story of what the feature asked for and what
            // it was told.
            if (TRACE != null) TRACE.merge(n, 1, Integer::sum);
            switch (n) {
                case "getBlockState": {
                    BlockPos p = (BlockPos) args[0];
                    ChunkAccess c = at(chunk, hood, p, minX, minZ);
                    return c == null ? Blocks.AIR.defaultBlockState() : c.getBlockState(p);
                }
                case "getFluidState": {
                    BlockPos p = (BlockPos) args[0];
                    ChunkAccess c = at(chunk, hood, p, minX, minZ);
                    return c == null ? Blocks.AIR.defaultBlockState().getFluidState()
                                     : c.getFluidState(p);
                }
                case "setBlock": {
                    BlockPos p = (BlockPos) args[0];
                    // With a neighbourhood these route to the owning chunk, as
                    // vanilla does. Without one they are dropped rather than
                    // misapplied: ProtoChunk masks x/z by 15, so writing a
                    // neighbour position into this chunk would silently corrupt
                    // a block on the far side of it.
                    ChunkAccess c = at(chunk, hood, p, minX, minZ);
                    if (c == null) return false;
                    c.setBlockState(p, (BlockState) args[1], false);
                    return true;
                }
                case "getBlockEntity":   return null;
                // This one decides WHERE a feature may place. The recorder's
                // default of false would not crash -- it would quietly stop ore
                // being placed at all, and the whole path would look fast and
                // sane while answering "no ore" everywhere.
                case "isStateAtPosition": {
                    BlockPos p = (BlockPos) args[0];
                    @SuppressWarnings("unchecked")
                    java.util.function.Predicate<BlockState> pred =
                        (java.util.function.Predicate<BlockState>) args[1];
                    ChunkAccess c = at(chunk, hood, p, minX, minZ);
                    return pred.test(c == null ? Blocks.AIR.defaultBlockState()
                                               : c.getBlockState(p));
                }
                case "isFluidAtPosition": {
                    BlockPos p = (BlockPos) args[0];
                    @SuppressWarnings("unchecked")
                    java.util.function.Predicate<net.minecraft.world.level.material.FluidState> pred =
                        (java.util.function.Predicate<net.minecraft.world.level.material.FluidState>) args[1];
                    ChunkAccess c = at(chunk, hood, p, minX, minZ);
                    return pred.test(c == null
                        ? Blocks.AIR.defaultBlockState().getFluidState()
                        : c.getFluidState(p));
                }
                case "getFluidTicks":
                case "getBlockTicks":
                    return net.minecraft.world.ticks.BlackholeTickAccess.emptyLevelList();
                case "getLevelData":     return LEVEL_DATA;
                case "nextSubTickCount": return 0L;
                // Light is FAKED: full sky, no block light. Ore placement does
                // not consult it, so the answers this class exists to give are
                // unaffected -- but light-gated features (vegetation, mostly)
                // may place differently here than in the real world. That is an
                // accepted deviation for an ore query and would not be one for
                // anything reading the surface.
                case "getBrightness":            return 15;
                case "getRawBrightness":         return 15;
                case "getMaxLocalRawBrightness": return 15;
                case "canSeeSky":                return true;
                case "getShade":                 return 1.0f;
                case "getDifficulty":
                    return net.minecraft.world.Difficulty.NORMAL;
                case "dimensionType":
                    return OutpostWorldgen.registries()
                        .registryOrThrow(net.minecraft.core.registries.Registries.DIMENSION_TYPE)
                        .get(net.minecraft.world.level.dimension.BuiltinDimensionTypes.OVERWORLD);
                case "getChunk":
                    // Neighbours return an EMPTY chunk, never this one and never
                    // null. Null makes callers NPE on getSections(); handing back
                    // our own chunk is worse than that, because ProtoChunk masks
                    // x/z by 15 and would silently answer with the wrong block
                    // wrapped round from inside this chunk. Empty reads as air,
                    // which is honestly "nothing here", and drops writes.
                    if (args.length >= 2 && args[0] instanceof Integer) {
                        int cx = (Integer) args[0], cz = (Integer) args[1];
                        if (cx == cp.x && cz == cp.z) return chunk;
                        if (hood != null) {
                            ChunkAccess nb = hood.get(key(cx, cz));
                            if (nb != null) return nb;
                        }
                        return emptyChunk(cx, cz, chunk, biomeReg);
                    }
                    return chunk;
                case "hasChunk":
                    return true;
                case "getHeight":
                    if (args != null && args.length == 3)
                        return chunk.getHeight((Heightmap.Types) args[0],
                                               (Integer) args[1], (Integer) args[2]);
                    return chunk.getHeight();
                case "getMinBuildHeight": return chunk.getMinBuildHeight();
                case "getMaxBuildHeight": return chunk.getMaxBuildHeight();
                case "getSectionsCount":  return chunk.getSectionsCount();
                case "getMinSection":     return chunk.getMinSection();
                case "getSeed":           return seed;
                case "getBiomeManager":   return biomes;
                case "registryAccess":    return OutpostWorldgen.registries();
                case "enabledFeatures":
                    return net.minecraft.world.flag.FeatureFlags.VANILLA_SET;
                case "getLevel":          return null;
                case "getServer":         return null;
                case "isClientSide":      return false;
                case "getSeaLevel":       return 63;
                case "getChunkSource":    return null;
                case "getLightEngine":    return null;
                case "addFreshEntity":    return false;    // no entities headless
                case "getUncachedNoiseBiome":
                    return gen.getBiomeSource().getNoiseBiome(
                        (Integer) args[0], (Integer) args[1], (Integer) args[2],
                        rs.sampler());
                case "toString":          return "OreGen.WorldGenLevel";
                case "hashCode":          return System.identityHashCode(proxy);
                case "equals":            return proxy == args[0];
                default:
                    break;
            }
            // Anything with an interface default -- the great majority of this
            // type -- gets the interface's own behaviour, built on the methods
            // above.
            if (method.isDefault())
                return InvocationHandler.invokeDefault(proxy, method, args);
            return unimplemented(method);
        };
        return (WorldGenLevel) Proxy.newProxyInstance(
            OreGen.class.getClassLoader(), new Class<?>[]{WorldGenLevel.class}, h);
    }

    // Enough LevelData for decoration: no weather, day 0, default rules. Ore
    // placement does not read any of it, but something on the path asks.
    static final net.minecraft.world.level.storage.LevelData LEVEL_DATA =
        new net.minecraft.world.level.storage.LevelData() {
            final net.minecraft.world.level.GameRules rules =
                new net.minecraft.world.level.GameRules();
            public BlockPos getSpawnPos() { return BlockPos.ZERO; }
            public float getSpawnAngle() { return 0f; }
            public long getGameTime() { return 0L; }
            public long getDayTime() { return 0L; }
            public boolean isThundering() { return false; }
            public boolean isRaining() { return false; }
            public void setRaining(boolean b) { }
            public boolean isHardcore() { return false; }
            public net.minecraft.world.level.GameRules getGameRules() { return rules; }
            public net.minecraft.world.Difficulty getDifficulty() {
                return net.minecraft.world.Difficulty.NORMAL;
            }
            public boolean isDifficultyLocked() { return false; }
        };

    static final Map<Long, ProtoChunk> EMPTY = new java.util.HashMap<>();

    static ProtoChunk emptyChunk(int cx, int cz, ChunkAccess like,
                                 Registry<Biome> biomeReg) {
        long k = ((long) cx << 32) ^ (cz & 0xffffffffL);
        ProtoChunk pc = EMPTY.get(k);
        if (pc == null) {
            pc = new ProtoChunk(new ChunkPos(cx, cz),
                                net.minecraft.world.level.chunk.UpgradeData.EMPTY,
                                like, biomeReg, null);
            EMPTY.put(k, pc);
        }
        return pc;
    }

    static long key(int cx, int cz) {
        return ((long) cx << 32) ^ (cz & 0xffffffffL);
    }

    /** The chunk owning `p`: this one, a neighbour, or null for "not available". */
    static ChunkAccess at(ChunkAccess self, Map<Long, ChunkAccess> hood,
                          BlockPos p, int minX, int minZ) {
        if (inChunk(p, minX, minZ)) return self;
        if (hood == null) return null;
        return hood.get(key(p.getX() >> 4, p.getZ() >> 4));
    }

    static boolean inChunk(BlockPos p, int minX, int minZ) {
        return p.getX() >= minX && p.getX() < minX + 16
            && p.getZ() >= minZ && p.getZ() < minZ + 16;
    }

    // Recorded, never silently defaulted. A feature that needs something absent
    // here would otherwise get a null or a zero and place ore in the wrong
    // place, which is the one failure this whole path cannot tolerate: an answer
    // that is fast and wrong is worse than the server that is slow and right.
    static Object unimplemented(Method m) {
        UNIMPLEMENTED.add(m.getName() + " -> " + m.getReturnType().getSimpleName());
        Class<?> r = m.getReturnType();
        if (r == boolean.class) return false;
        if (r == int.class) return 0;
        if (r == long.class) return 0L;
        if (r == float.class) return 0f;
        if (r == double.class) return 0d;
        if (r == void.class) return null;
        return null;
    }

    /** Terrain + surface + decoration (ores included), for one chunk. */
    static ChunkAccess genDecoratedChunk(long seed, int chunkX, int chunkZ) {
        EMPTY.clear();
        return decorate(seed, OutpostWorldgen.genFullChunk(seed, chunkX, chunkZ));
    }

    /** Run applyBiomeDecoration over a chunk the caller may already have written to.
     *
     *  Split out because ORDER IS PART OF THE ANSWER. Vanilla places buried
     *  treasure in the SURFACE_STRUCTURES step and ore in UNDERGROUND_ORES,
     *  which runs later -- so the chest is positioned against terrain that has
     *  no ore in it yet, and ore may then overwrite the very block that stopped
     *  its descent. Decorating first and placing afterwards inverts that and
     *  moves chests. Callers that care must place, then call this.
     */
    /** Generate the 3x3 around (cx,cz), decorate ALL of them, return the centre.
     *
     *  Vanilla decorates every chunk against a WorldGenRegion spanning its
     *  neighbours, so a blob whose origin is next door writes across the border
     *  into this chunk. Decorating one chunk in isolation cannot produce those
     *  blocks at all, which is the standing explanation for the face
     *  disagreements against the server. This runs all nine, with reads and
     *  writes routed to the owning chunk, and returns the centre.
     *
     *  Nine times the work, so it is opt-in rather than the default.
     */
    static ChunkAccess decorateWithNeighbours(long seed, int cx, int cz,
                                              ChunkAccess centre) {
        Map<Long, ChunkAccess> hood = new java.util.HashMap<>();
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dz == 0) { hood.put(key(cx, cz), centre); continue; }
                hood.put(key(cx + dx, cz + dz),
                         OutpostWorldgen.genFullChunk(seed, cx + dx, cz + dz));
            }
        // Neighbours first, centre last. A neighbour decorating after the
        // centre would write its cross-border blobs into a chunk we have
        // already read, which is the same ordering error as decorating before
        // placing -- just one level out.
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++)
                if (dx != 0 || dz != 0)
                    decorateIn(seed, hood.get(key(cx + dx, cz + dz)), hood);
        decorateIn(seed, centre, hood);
        return centre;
    }

    static ChunkAccess decorate(long seed, ChunkAccess chunk) {
        return decorateIn(seed, chunk, null);
    }

    static ChunkAccess decorateIn(long seed, ChunkAccess chunk,
                                  Map<Long, ChunkAccess> hood) {
        // Cleared per call, for two reasons and the second is the bad one.
        // It grows by a ProtoChunk per neighbour per chest, which ran a census
        // out of heap at 2918 chests. And it was keyed on chunk coordinates
        // ALONE, so the same entry was handed to a different seed later: any
        // feature writing into a neighbour chunk would leave blocks behind that
        // the next seed's decoration would then read as its own terrain. A slow
        // leak is obvious when it kills the run; stale world state is not.
        NoiseBasedChunkGenerator gen = OutpostWorldgen.cachedGen;
        RandomState rs = RandomState.create(
            OutpostWorldgen.noiseSettings.value(),
            OutpostWorldgen.registries().lookupOrThrow(Registries.NOISE), seed);
        Registry<Biome> biomeReg =
            OutpostWorldgen.registries().registryOrThrow(Registries.BIOME);
        // obfuscateSeed, NOT the raw seed. WorldGenRegion's constructor builds
        // its BiomeManager as new BiomeManager(this, obfuscateSeed(seed)), and
        // BiomeManager uses that seed for the per-block fuzzy offset in
        // getBiome(BlockPos). Surface rules consult getBiome, so a raw seed
        // resolves some individual blocks to a different biome and lays down
        // different sediment -- which is exactly the sand/gravel/dirt/sandstone
        // disagreement measured against the server.
        BiomeManager bm = new BiomeManager(
            (x, y, z) -> gen.getBiomeSource().getNoiseBiome(x, y, z, rs.sampler()),
            BiomeManager.obfuscateSeed(seed));
        // generateStructures MUST be true, and this is not a detail.
        // applyBiomeDecoration advances one shared counter k across a step:
        // first over every structure REGISTERED for that step, then over that
        // step's features, calling setFeatureSeed(decorationSeed, k, step) each
        // time. So the structure loop is what determines which seed each ore
        // feature gets. Skipping it -- which is what shouldGenerateStructures()
        // returning false does -- shifts every subsequent index and places ore
        // somewhere the real game never would, silently. The count comes from
        // the registry rather than from what is actually present, so our
        // no-starts stub does not disturb it.
        net.minecraft.world.level.levelgen.WorldOptions opts =
            new net.minecraft.world.level.levelgen.WorldOptions(seed, true, false);
        // The level is built first because StructureManager keeps its own
        // reference and dereferences it; passing null there is what the
        // "this.level is null" failure was.
        WorldGenLevel level = levelFor(seed, chunk, gen, rs, bm, biomeReg, hood);
        StructureManager sm = new StructureManager(level, opts, null) {
            public List<StructureStart> startsForStructure(
                    ChunkPos p, java.util.function.Predicate<Structure> pred) {
                return List.of();
            }
            public boolean hasAnyStructureAt(BlockPos p) { return false; }
            public Map<Structure, it.unimi.dsi.fastutil.longs.LongSet>
                    getAllStructuresAt(BlockPos p) { return Map.of(); }
        };
        gen.applyBiomeDecoration(level, chunk, sm);
        return chunk;
    }


    /** 1 = terrain, 0 = air/fluid, indexed from world y=-64. Real game terrain. */
    static int[] column(long seed, int x, int z) {
        ChunkAccess c = OutpostWorldgen.genFullChunk(seed, x >> 4, z >> 4);
        int[] out = new int[384];
        for (int i = 0; i < 384; i++) {
            BlockState st = c.getBlockState(new BlockPos(x, i - 64, z));
            // Fluids are not ground: a seabed under 30 blocks of water is not a
            // floating island, and counting water as solid would call it one.
            out[i] = (st.isAir() || !st.getFluidState().isEmpty()) ? 0 : 1;
        }
        return out;
    }

    static int topSolid(int[] col) {
        for (int i = col.length - 1; i >= 0; i--) if (col[i] == 1) return i;
        return -1;
    }

    public static void main(String[] argv) throws Exception {
        OutpostWorldgen.bootstrapRegistries();
        if (argv.length > 0 && argv[0].equals("floatserver")) {
            // "seed x z gap" -> is (x,z) a FLOATING island, from the GAME'S OWN
            // terrain rather than a reimplementation of it.
            //
            // Three earlier attempts asked a running server through chat
            // commands and all three failed: a full sweep lost results when ~260
            // heights went out in one batch, and a binary search is structurally
            // invalid because an island makes the column non-monotone (solid,
            // air, solid) which is exactly what bisection cannot handle.
            //
            // None of that was necessary. This backend already runs Minecraft's
            // own NoiseBasedChunkGenerator, so the column is an array in memory:
            // no commands, no batching limit, no cubiomes. Reading it directly is
            // both exact and faster than either attempt.
            Bootstrap0.println("READY");
            java.io.BufferedReader in = new java.io.BufferedReader(
                new java.io.InputStreamReader(System.in));
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                if (line.equals("quit")) break;
                String[] t = line.split("\s+");
                long s = Long.parseLong(t[0]);
                int x = Integer.parseInt(t[1]), z = Integer.parseInt(t[2]);
                int gap = t.length > 3 ? Integer.parseInt(t[3]) : 24;
                StringBuilder sb = new StringBuilder();
                try {
                    int[] c = column(s, x, z);
                    int top = topSolid(c);
                    if (top < 0) {
                        sb.append("F	").append(s).append('	').append(x)
                          .append('	').append(z).append("	empty=1");
                    } else {
                        int base = top;
                        while (base > 0 && c[base - 1] == 1) base--;
                        int voidH = 0, y = base - 1;
                        while (y >= 0 && c[y] == 0) { voidH++; y--; }
                        int capY = base - 64, topY = top - 64;
                        // The ring decides island vs cave roof: ground that
                        // continues outward at the cap's height is not an island,
                        // however deep the cavern under it happens to be.
                        int lower = 0;
                        int[][] dir = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
                        StringBuilder ring = new StringBuilder();
                        for (int[] d : dir) {
                            int[] rc = column(s, x + d[0]*gap, z + d[1]*gap);
                            int rt = topSolid(rc);
                            int rtY = rt < 0 ? -999 : rt - 64;
                            if (rt >= 0 && rt < base) lower++;
                            ring.append(',').append(rtY);
                        }
                        sb.append("F	").append(s).append('	').append(x)
                          .append('	').append(z)
                          .append("	top=").append(topY)
                          .append("	cap=").append(capY)
                          .append("	void=").append(voidH)
                          .append("	lower=").append(lower)
                          .append("	ring=").append(ring.substring(1));
                    }
                } catch (Throwable ex) {
                    sb.setLength(0);
                    sb.append("F	").append(s).append('	').append(x)
                      .append('	').append(z).append("	err=").append(ex);
                }
                Bootstrap0.println(sb.toString());
            }
            return;
        }
        if (argv.length > 0 && argv[0].equals("column")) {
            // "column <seed> <x> <z> <y0> <y1>": one column, terrain-only beside
            // decorated. Buried treasure is placed in the SURFACE_STRUCTURES
            // step and ore in UNDERGROUND_ORES, which runs LATER -- so the block
            // under a finished chest is not necessarily the block that stopped
            // its descent. This shows both, which is the only way to tell a
            // placement-rule violation from a later overwrite.
            long s = Long.parseLong(argv[1]);
            int x = Integer.parseInt(argv[2]), z = Integer.parseInt(argv[3]);
            int y0 = Integer.parseInt(argv[4]), y1 = Integer.parseInt(argv[5]);
            ChunkAccess plain = OutpostWorldgen.genFullChunk(s, x >> 4, z >> 4);
            java.util.List<String> before = new java.util.ArrayList<>();
            for (int y = y1; y >= y0; y--)
                before.add(name(plain.getBlockState(new BlockPos(x, y, z))));
            ChunkAccess dec = genDecoratedChunk(s, x >> 4, z >> 4);
            System.out.println("y	terrain-only	decorated");
            for (int y = y1, i = 0; y >= y0; y--, i++) {
                String a = before.get(i);
                String b = name(dec.getBlockState(new BlockPos(x, y, z)));
                System.out.println(y + "	" + a + "	" + b
                    + (a.equals(b) ? "" : "	<== CHANGED BY DECORATION"));
            }
            return;
        }
        if (argv.length > 0 && argv[0].equals("tags")) {
            // Every ore's RuleTest is TagMatchTest(STONE_ORE_REPLACEABLES) or
            // its deepslate twin, and TagMatchTest.test is just state.is(tag).
            // An unbound tag makes that false for every block in the world,
            // which is indistinguishable from "there is no ore here" -- the
            // feature reads the stone, rejects it, and writes nothing.
            for (Object[] t : new Object[][]{
                    {"STONE_ORE_REPLACEABLES", net.minecraft.tags.BlockTags.STONE_ORE_REPLACEABLES},
                    {"DEEPSLATE_ORE_REPLACEABLES", net.minecraft.tags.BlockTags.DEEPSLATE_ORE_REPLACEABLES},
                    {"BASE_STONE_OVERWORLD", net.minecraft.tags.BlockTags.BASE_STONE_OVERWORLD},
                    {"DIRT", net.minecraft.tags.BlockTags.DIRT}}) {
                @SuppressWarnings("unchecked")
                net.minecraft.tags.TagKey<net.minecraft.world.level.block.Block> key =
                    (net.minecraft.tags.TagKey<net.minecraft.world.level.block.Block>) t[1];
                long size = net.minecraft.core.registries.BuiltInRegistries.BLOCK
                    .getTag(key).map(h -> (long) h.size()).orElse(-1L);
                System.out.println(t[0] + ": members=" + size
                    + "  stone.is()=" + Blocks.STONE.defaultBlockState().is(key)
                    + "  deepslate.is()=" + Blocks.DEEPSLATE.defaultBlockState().is(key));
            }
            return;
        }
        if (argv.length > 0 && argv[0].equals("orefeature")) {
            // "orefeature <seed> <x> <z> <placed_feature_id>": place ONE named
            // placed feature onto the proxy level by hand and diff the chunk.
            //
            // This bisects the failure. applyBiomeDecoration does two separable
            // things -- decide WHICH features to run, then run them -- and the
            // chunk diff cannot tell which half is broken. Calling the feature
            // directly answers it: if ore places here, the features work and the
            // enumeration is dropping them; if it does not, the feature itself
            // cannot run against this proxy.
            long s = Long.parseLong(argv[1]);
            int x = Integer.parseInt(argv[2]), z = Integer.parseInt(argv[3]);
            String[] ids = argv[4].split(",");  // several per JVM boot: the boot dominates
            int cx = x >> 4, cz = z >> 4;
            EMPTY.clear();
            ChunkAccess chunk = OutpostWorldgen.genFullChunk(s, cx, cz);
            TreeMap<String, Integer> before = tallyChunk(chunk, cx, cz);
            NoiseBasedChunkGenerator gen = OutpostWorldgen.cachedGen;
            RandomState rs = RandomState.create(
                OutpostWorldgen.noiseSettings.value(),
                OutpostWorldgen.registries().lookupOrThrow(Registries.NOISE), s);
            Registry<Biome> biomeReg =
                OutpostWorldgen.registries().registryOrThrow(Registries.BIOME);
            BiomeManager bm = new BiomeManager(
                (bx, by, bz) -> gen.getBiomeSource().getNoiseBiome(bx, by, bz, rs.sampler()),
                BiomeManager.obfuscateSeed(s));
            WorldGenLevel level = levelFor(s, chunk, gen, rs, bm, biomeReg);
            for (String id : ids) {
                Registry<net.minecraft.world.level.levelgen.placement.PlacedFeature> pfReg =
                    OutpostWorldgen.registries().registryOrThrow(Registries.PLACED_FEATURE);
                net.minecraft.world.level.levelgen.placement.PlacedFeature pf =
                    pfReg.get(net.minecraft.resources.ResourceLocation.parse(id));
                if (pf == null) {
                    System.out.println(id + ": NO SUCH PLACED FEATURE");
                    continue;
                }
                System.out.println("--- " + id + " ---");
                // Which biome is actually here, and does its generation settings
                // list this feature at all? A feature absent from the biome is a
                // legitimate reason for zero, and must not read as a broken proxy.
                Holder<Biome> biome = gen.getBiomeSource().getNoiseBiome(
                    x >> 2, 64 >> 2, z >> 2, rs.sampler());
                boolean listed = biome.value().getGenerationSettings().features().stream()
                    .flatMap(hs -> java.util.stream.StreamSupport.stream(hs.spliterator(), false))
                    .anyMatch(h -> h.value() == pf);
                System.out.println("biome=" + biome.unwrapKey().map(Object::toString).orElse("?")
                    + "  feature_listed_in_biome=" + listed);
                // Walk the placement modifiers one at a time. placeWithBiomeCheck
                // collapses the whole pipeline into a single boolean, and "false"
                // is the same answer whether a rarity filter rolled badly or the
                // proxy is starving a modifier -- so it names nothing. Counting the
                // positions in and out of each stage says exactly which one is
                // eating them.
                net.minecraft.world.level.levelgen.placement.PlacementContext ctx =
                    new net.minecraft.world.level.levelgen.placement.PlacementContext(
                        level, gen, java.util.Optional.of(pf));
                net.minecraft.util.RandomSource rnd =
                    net.minecraft.util.RandomSource.create(s);
                java.util.List<BlockPos> cur =
                    java.util.List.of(new BlockPos(cx * 16, 0, cz * 16));
                for (net.minecraft.world.level.levelgen.placement.PlacementModifier mod
                         : pf.placement()) {
                    java.util.List<BlockPos> next = new java.util.ArrayList<>();
                    for (BlockPos p : cur) mod.getPositions(ctx, rnd, p).forEach(next::add);
                    System.out.println("  " + mod.getClass().getSimpleName()
                        + ": " + cur.size() + " -> " + next.size()
                        + (next.isEmpty() ? "   <== KILLED HERE" : ""));
                    cur = next;
                    if (cur.isEmpty()) break;
                }
                // How many surviving positions are even in rock? An ore feature
                // aimed above the terrain places nothing and is not broken -- the
                // upper iron band does exactly that most of the time. Separating
                // "aimed at air" from "aimed at stone and refused" is the whole
                // difference between normal and a bug.
                int inRock = 0;
                for (BlockPos p : cur) {
                    BlockState st = chunk.getBlockState(p);
                    if (!st.isAir() && st.getFluidState().isEmpty()) inRock++;
                }
                System.out.println("  surviving positions: " + cur.size()
                    + ", of which in solid rock: " + inRock);
                for (int i = 0; i < Math.min(4, cur.size()); i++)
                    System.out.println("    " + cur.get(i) + " = "
                        + name(chunk.getBlockState(cur.get(i))));
                // The heightmap gate in OreFeature.place is the one call that
                // can reject a position sitting in solid stone, so read it
                // through the proxy exactly as the feature does.
                if (!cur.isEmpty()) {
                    BlockPos q = cur.get(0);
                    System.out.println("  proxy getHeight(OCEAN_FLOOR_WG," + q.getX()
                        + "," + q.getZ() + ") = "
                        + level.getHeight(Heightmap.Types.OCEAN_FLOOR_WG, q.getX(), q.getZ())
                        + "   (feature needs this >= about y" + (q.getY() - 4) + ")");
                    System.out.println("  proxy getMinSection=" + level.getMinSection()
                        + " getSectionsCount=" + level.getSectionsCount()
                        + " getSectionIndex(" + q.getY() + ")=" + level.getSectionIndex(q.getY()));
                }
                TRACE = new TreeMap<>();
                boolean placed = pf.placeWithBiomeCheck(level, gen,
                    net.minecraft.util.RandomSource.create(s), new BlockPos(cx * 16, 0, cz * 16));
                System.out.println("  proxy calls: " + TRACE);
                TRACE = null;
                TreeMap<String, Integer> after = tallyChunk(chunk, cx, cz);
                java.util.TreeSet<String> keys = new java.util.TreeSet<>(before.keySet());
                keys.addAll(after.keySet());
                System.out.println("place() returned " + placed);
                for (String k : keys) {
                    int b = before.getOrDefault(k, 0), a = after.getOrDefault(k, 0);
                    if (a != b) System.out.println("  " + k + "  " + b + " -> " + a
                                                   + "  (" + (a - b > 0 ? "+" : "") + (a - b) + ")");
                }

            }
            if (!UNIMPLEMENTED.isEmpty())
                System.out.println("UNIMPLEMENTED: " + UNIMPLEMENTED);
            return;
        }
        if (argv.length > 0 && argv[0].equals("orescan")) {
            // "orescan <seed> <x> <z>": tally EVERY block in the decorated
            // chunk. This exists because a census of 8057 chests found ore on
            // 0 of 48342 casing faces, and near-zero andesite/granite/diorite
            // anywhere -- and "no ore in the world" and "no ore being placed by
            // this path" produce the identical readout. A zero from a decorator
            // that never ran is not a fact about Minecraft. This says which it
            // is by counting the chunk itself rather than the six blocks the
            // chest happens to touch.
            long s = Long.parseLong(argv[1]);
            int x = Integer.parseInt(argv[2]), z = Integer.parseInt(argv[3]);
            // Tallied BOTH ways. A decorated chunk on its own cannot tell the
            // difference between "the decorator placed nothing" and "the
            // decorator never ran": both hand back terrain. The undecorated
            // chunk is the control, and the diff is the only honest measure of
            // what applyBiomeDecoration actually contributed.
            ChunkAccess plain = OutpostWorldgen.genFullChunk(s, x >> 4, z >> 4);
            TreeMap<String, Integer> before = tallyChunk(plain, x >> 4, z >> 4);
            ChunkAccess c = genDecoratedChunk(s, x >> 4, z >> 4);
            TreeMap<String, Integer> after = tallyChunk(c, x >> 4, z >> 4);
            java.util.TreeSet<String> all = new java.util.TreeSet<>(before.keySet());
            all.addAll(after.keySet());
            System.out.println("block\tbefore\tafter\tdelta");
            int changed = 0;
            for (String k : all) {
                int b = before.getOrDefault(k, 0), a = after.getOrDefault(k, 0);
                if (a != b) changed++;
                System.out.println(k + "\t" + b + "\t" + a + "\t" + (a - b));
            }
            System.out.println("blocks whose count changed: " + changed);
            if (!UNIMPLEMENTED.isEmpty())
                System.out.println("UNIMPLEMENTED: " + UNIMPLEMENTED);
            return;
        }
        if (argv.length > 0 && argv[0].equals("oreserver")) {
            // "seed x z" -> chest Y and the six face blocks, WITH ores.
            // "oreserver hood" decorates the full 3x3 instead of one chunk.
            final boolean HOOD = argv.length > 1 && argv[1].equals("hood");
            Bootstrap0.println("READY");
            java.io.BufferedReader in = new java.io.BufferedReader(
                new java.io.InputStreamReader(System.in));
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                if (line.equals("quit")) break;
                String[] t = line.split("\\s+");
                long s = Long.parseLong(t[0]);
                int x = Integer.parseInt(t[1]), z = Integer.parseInt(t[2]);
                StringBuilder sb = new StringBuilder();
                try {
                    // ORDER MATTERS AND THIS HAD IT BACKWARDS.
                    // Vanilla runs SURFACE_STRUCTURES (buried treasure) before
                    // UNDERGROUND_ORES, so the chest is positioned against
                    // terrain with no ore in it, and ore is free to overwrite
                    // the block that stopped the descent afterwards. This
                    // decorated first and scanned second, so wherever an ore
                    // blob had replaced the floor the scan fell straight through
                    // it -- ore is not a stopping block -- and landed too low.
                    //
                    // Measured at seed 20094 (-1783,41): terrain-only y49 is
                    // stone, decorated y49 is coal_ore. Vanilla stops at y=50,
                    // where the server really has the chest; scanning the
                    // decorated chunk fell through y49 and y48 and answered 48.
                    //
                    // Invisible until the tag fix, because a decorator that
                    // places no ore makes the decorated chunk identical to the
                    // plain one and the order cannot matter.
                    EMPTY.clear();
                    ChunkAccess c = OutpostWorldgen.genFullChunk(s, x >> 4, z >> 4);
                    int y = OutpostWorldgen.chestPlacementY(c, x, z);
                    sb.append("O	").append(s).append('	').append(x)
                      .append('	').append(z).append("	chestY=").append(y);
                    BlockState atPos = c.getBlockState(new BlockPos(x, y, z));
                    BlockState under = c.getBlockState(new BlockPos(x, y - 1, z));
                    // From the game's bytecode, not inferred: the piece uses a
                    // HARDCODED Blocks.SAND when the landing block is air or
                    // liquid. It does NOT fall back to the block below. That one
                    // constant is why sand dominates the census -- every chest
                    // landing in water is cased in sand whatever it sits on.
                    BlockState fillState =
                        (atPos.isAir() || isLiquid(atPos))
                            ? Blocks.SAND.defaultBlockState() : atPos;
                    sb.append("	fill=").append(name(fillState));
                    // `floor` is not free: the scan only STOPS where the block
                    // below is sandstone/stone/andesite/granite/diorite, so at
                    // PLACEMENT TIME it is always one of those five and can
                    // never be ore. What sits under a finished chest in the
                    // world may well be ore, because decoration gets there
                    // afterwards. Different questions; this is the first.
                    sb.append("	floor=").append(name(under));
                    int[][] f = {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
                    StringBuilder raw = new StringBuilder(), cased = new StringBuilder();
                    // The casing is WRITTEN INTO THE CHUNK, not merely computed,
                    // so that decoration afterwards sees what the piece built
                    // and can overwrite it exactly as it does in a real world.
                    for (int[] d : f) {
                        BlockPos fp = new BlockPos(x + d[0], y + d[1], z + d[2]);
                        BlockState fs = c.getBlockState(fp);
                        BlockState beneath = c.getBlockState(fp.below());
                        BlockState out;
                        if (!(fs.isAir() || isLiquid(fs))) {
                            out = fs;                       // solid: untouched
                        } else if ((beneath.isAir() || isLiquid(beneath))
                                   && d[1] != 1) {          // overhang, not UP
                            out = under;                    // the floor block
                        } else {
                            out = fillState;
                        }
                        if (out != fs) c.setBlockState(fp, out, false);
                        raw.append(',').append(name(fs));
                        cased.append(',').append(name(out));
                    }
                    sb.append("	raw=").append(raw.substring(1));
                    sb.append("	cased=").append(cased.substring(1));
                    // ...and only now the ores. `final` is what a player digs
                    // up; `cased` is what the structure wrote. They differ
                    // wherever a later feature landed on the chest, which is a
                    // real thing the game does and not an error.
                    if (HOOD) decorateWithNeighbours(s, x >> 4, z >> 4, c);
                    else decorate(s, c);
                    StringBuilder fin = new StringBuilder();
                    for (int[] d : f)
                        fin.append(',').append(name(c.getBlockState(
                            new BlockPos(x + d[0], y + d[1], z + d[2]))));
                    sb.append("	final=").append(fin.substring(1));
                } catch (Throwable ex) {
                    if (System.getenv("OREGEN_TRACE") != null) ex.printStackTrace();
                    sb.setLength(0);
                    sb.append("O\t").append(s).append('\t').append(x)
                      .append('\t').append(z).append("\terr=").append(ex);
                }
                Bootstrap0.println(sb.toString());
            }
            if (!UNIMPLEMENTED.isEmpty())
                System.err.println("UNIMPLEMENTED: " + UNIMPLEMENTED);
            return;
        }
        System.out.println("usage: OreGen oreserver");
    }

    // BuriedTreasurePiece.isLiquid, transcribed from its bytecode: an identity
    // test against WATER and LAVA's DEFAULT states, and nothing else. This path
    // used !getFluidState().isEmpty() instead, which is a different question and
    // a wider one -- kelp, seagrass and every waterlogged block carry a fluid
    // state while failing the game's test. Those landed here as "air or water"
    // and were replaced by the hardcoded SAND, when the game would have taken
    // the block itself as the fill. Some of the 22% fill disagreement with the
    // server is this, not decoration.
    static boolean isLiquid(BlockState st) {
        return st == Blocks.WATER.defaultBlockState()
            || st == Blocks.LAVA.defaultBlockState();
    }

    static TreeMap<String, Integer> tallyChunk(ChunkAccess c, int cx, int cz) {
        TreeMap<String, Integer> t = new TreeMap<>();
        for (int bx = 0; bx < 16; bx++)
          for (int bz = 0; bz < 16; bz++)
            for (int by = -64; by < 200; by++)
                t.merge(name(c.getBlockState(
                    new BlockPos(cx * 16 + bx, by, cz * 16 + bz))), 1, Integer::sum);
        return t;
    }

    static String name(BlockState st) {
        return net.minecraft.core.registries.BuiltInRegistries.BLOCK
            .getKey(st.getBlock()).toString();
    }

    static class Bootstrap0 {
        static void println(String s) {
            net.minecraft.server.Bootstrap.STDOUT.println(s);
            net.minecraft.server.Bootstrap.STDOUT.flush();
        }
    }
}
