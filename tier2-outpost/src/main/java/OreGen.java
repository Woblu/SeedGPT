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

    /** A WorldGenLevel backed by one ProtoChunk. */
    static WorldGenLevel levelFor(long seed, ChunkAccess chunk,
                                  NoiseBasedChunkGenerator gen, RandomState rs,
                                  BiomeManager biomes, Registry<Biome> biomeReg) {
        ChunkPos cp = chunk.getPos();
        int minX = cp.getMinBlockX(), minZ = cp.getMinBlockZ();

        InvocationHandler h = (proxy, method, args) -> {
            String n = method.getName();
            switch (n) {
                case "getBlockState": {
                    BlockPos p = (BlockPos) args[0];
                    if (!inChunk(p, minX, minZ)) return Blocks.AIR.defaultBlockState();
                    return chunk.getBlockState(p);
                }
                case "getFluidState": {
                    BlockPos p = (BlockPos) args[0];
                    if (!inChunk(p, minX, minZ)) return Blocks.AIR.defaultBlockState().getFluidState();
                    return chunk.getFluidState(p);
                }
                case "setBlock": {
                    BlockPos p = (BlockPos) args[0];
                    // Writes outside this chunk are dropped rather than
                    // misapplied. Vanilla would route them to the neighbour.
                    if (!inChunk(p, minX, minZ)) return false;
                    chunk.setBlockState(p, (BlockState) args[1], false);
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
                    return pred.test(inChunk(p, minX, minZ)
                        ? chunk.getBlockState(p) : Blocks.AIR.defaultBlockState());
                }
                case "isFluidAtPosition": {
                    BlockPos p = (BlockPos) args[0];
                    @SuppressWarnings("unchecked")
                    java.util.function.Predicate<net.minecraft.world.level.material.FluidState> pred =
                        (java.util.function.Predicate<net.minecraft.world.level.material.FluidState>) args[1];
                    return pred.test(inChunk(p, minX, minZ)
                        ? chunk.getFluidState(p)
                        : Blocks.AIR.defaultBlockState().getFluidState());
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
        ChunkAccess chunk = OutpostWorldgen.genFullChunk(seed, chunkX, chunkZ);
        NoiseBasedChunkGenerator gen = OutpostWorldgen.cachedGen;
        RandomState rs = RandomState.create(
            OutpostWorldgen.noiseSettings.value(),
            OutpostWorldgen.registries().lookupOrThrow(Registries.NOISE), seed);
        Registry<Biome> biomeReg =
            OutpostWorldgen.registries().registryOrThrow(Registries.BIOME);
        BiomeManager bm = new BiomeManager(
            (x, y, z) -> gen.getBiomeSource().getNoiseBiome(x, y, z, rs.sampler()),
            seed);
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
        WorldGenLevel level = levelFor(seed, chunk, gen, rs, bm, biomeReg);
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

    public static void main(String[] argv) throws Exception {
        OutpostWorldgen.bootstrapRegistries();
        if (argv.length > 0 && argv[0].equals("oreserver")) {
            // "seed x z" -> chest Y and the six face blocks, WITH ores.
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
                    ChunkAccess c = genDecoratedChunk(s, x >> 4, z >> 4);
                    int y = OutpostWorldgen.chestPlacementY(c, x, z);
                    sb.append("O\t").append(s).append('\t').append(x)
                      .append('\t').append(z).append("\tchestY=").append(y);
                    // The block the chest lands on IS the fill block, so it is
                    // reported directly rather than inferred from neighbours.
                    sb.append("\tfill=").append(name(c.getBlockState(new BlockPos(x, y, z))));
                    int[][] f = {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
                    for (int[] d : f)
                        sb.append('\t').append(name(
                            c.getBlockState(new BlockPos(x + d[0], y + d[1], z + d[2]))));
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
