import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.LayeredRegistryAccess;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.RegistryDataLoader;
import net.minecraft.resources.ResourceKey;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.Structure;
import net.minecraft.world.level.levelgen.structure.StructureStart;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureTemplateManager;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Registry;
import net.minecraft.world.level.StructureManager;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.WorldGenerationContext;
import net.minecraft.world.level.levelgen.blending.Blender;

import java.util.List;

// Headless MC 1.21.1: for a seed + a target (x,z) from the tier-1 finder, locate
// the pillager outpost, generate its REAL structure, and report how it sits on
// the terrain -- the ground truth for "tall/glitched" (stilted or floating).
public class OutpostWorldgen {

    static RegistryAccess.Frozen worldgen;
    static StructureTemplateManager templates;
    static Holder<NoiseGeneratorSettings> noiseSettings;
    static Holder<MultiNoiseBiomeSourceParameterList> owParams;
    static Holder.Reference<Structure> outpost;
    static boolean verbose = false;   // per-piece detail (single-seed mode only)

    static final LevelHeightAccessor OVERWORLD_HEIGHT = new LevelHeightAccessor() {
        public int getHeight() { return 384; }
        public int getMinBuildHeight() { return -64; }
    };

    // Extracted from main so a second entry point (OreGen) can bootstrap the
    // same registries without duplicating the datapack load, which is the bulk
    // of the 4.5s startup and is easy to get subtly wrong twice.
    static boolean booted = false;

    static void bootstrapRegistries() throws Exception {
        if (booted) return;
        booted = true;
        bootstrapBody();
    }

    /** The registries this backend was bootstrapped with. */
    static RegistryAccess.Frozen registries() { return worldgen; }

    public static void main(String[] args) throws Exception {
        bootstrapRegistries();
        mainBody(args);
    }

    static void bootstrapBody() throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        // Vanilla datapack -> resource manager -> worldgen registries (biomes,
        // structures, jigsaw pools, noise settings) loaded from the built-in JSON.
        PackRepository repo = ServerPacksSource.createVanillaTrustedRepository();
        repo.reload();
        repo.setSelected(repo.getAvailableIds());   // select ALL packs (esp. the core vanilla data)
        List<net.minecraft.server.packs.PackResources> packs = repo.openAllSelected();
        MultiPackResourceManager rm = new MultiPackResourceManager(PackType.SERVER_DATA, packs);
        LayeredRegistryAccess<RegistryLayer> layered = RegistryLayer.createRegistryAccess();
        RegistryAccess.Frozen wg = RegistryDataLoader.load(rm, layered.getAccessForLoading(RegistryLayer.WORLDGEN),
                                           RegistryDataLoader.WORLDGEN_REGISTRIES);
        // Composite so block/item (STATIC layer) AND biomes/structures (WORLDGEN) resolve.
        worldgen = layered.replaceFrom(RegistryLayer.WORLDGEN, wg).compositeAccess();

        // Bind TAGS. RegistryDataLoader loads registry CONTENTS; it does not
        // load tags, and nothing else here did either, so every tag was unbound
        // -- BuiltInRegistries.BLOCK.getTag(STONE_ORE_REPLACEABLES) was absent
        // entirely and state.is(anyTag) was false for every block in the world.
        //
        // That is not a cosmetic gap. Every ore's RuleTest is a TagMatchTest, so
        // an unbound tag makes each one reject stone it is standing in, and the
        // feature writes nothing and returns false. Decoration then produces
        // only the features whose configs use inline block lists rather than
        // tags -- springs and glow lichen -- which is exactly the ten-blocks-a-
        // chunk signature measured before this call existed. A whole subsystem
        // read as "this world has no ore".
        //
        // Bound for EVERY registry rather than just BLOCK on purpose: picking
        // the registry whose tags we happen to know about is how this failed in
        // the first place, and a feature consulting some other registry's tags
        // would fail the same silent way.
        bindAllTags(rm, worldgen);

        java.nio.file.Path tmp = java.nio.file.Files.createTempDirectory("owg");
        net.minecraft.world.level.storage.LevelStorageSource lss =
            net.minecraft.world.level.storage.LevelStorageSource.createDefault(tmp);
        net.minecraft.world.level.storage.LevelStorageSource.LevelStorageAccess lsa = lss.createAccess("w");
        HolderLookup.RegistryLookup<Block> blocks = worldgen.lookupOrThrow(Registries.BLOCK);
        templates = new StructureTemplateManager(rm, lsa, DataFixers.getDataFixer(), blocks);

        noiseSettings = worldgen.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(NoiseGeneratorSettings.OVERWORLD);
        owParams = worldgen.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST)
                           .getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);
        outpost = worldgen.lookupOrThrow(Registries.STRUCTURE)
                          .getOrThrow(ResourceKey.create(Registries.STRUCTURE,
                              ResourceLocation.withDefaultNamespace("pillager_outpost")));
    }

    /** Load and bind datapack tags for every registry in `ra`. */
    static void bindAllTags(net.minecraft.server.packs.resources.ResourceManager rm,
                            RegistryAccess.Frozen ra) {
        ra.registries().forEach(entry -> bindTagsFor(rm, entry));
    }

    private static <T> void bindTagsFor(net.minecraft.server.packs.resources.ResourceManager rm,
                                        RegistryAccess.RegistryEntry<T> entry) {
        ResourceKey<? extends net.minecraft.core.Registry<T>> key = entry.key();
        net.minecraft.core.Registry<T> reg = entry.value();
        if (!(reg instanceof net.minecraft.core.MappedRegistry)) return;
        net.minecraft.tags.TagLoader<net.minecraft.core.Holder<T>> loader =
            new net.minecraft.tags.TagLoader<>(
                id -> reg.getHolder(ResourceKey.create(
                          (ResourceKey<net.minecraft.core.Registry<T>>) key, id))
                         .map(h -> (net.minecraft.core.Holder<T>) h),
                Registries.tagsDirPath(key));
        java.util.Map<ResourceLocation, java.util.Collection<net.minecraft.core.Holder<T>>> built =
            loader.loadAndBuild(rm);
        java.util.Map<net.minecraft.tags.TagKey<T>,
                      List<net.minecraft.core.Holder<T>>> bound = new java.util.HashMap<>();
        built.forEach((id, members) -> bound.put(
            net.minecraft.tags.TagKey.create(key, id), List.copyOf(members)));
        ((net.minecraft.core.MappedRegistry<T>) reg).bindTags(bound);
    }

    static void mainBody(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("treasurefull")) {
            // Debug: FULL chunk gen (terrain + surface sand/gravel + water), then
            // dump the real block column around the chest so we can see the cover.
            long s = Long.parseLong(args[1]); int cx = (Integer.parseInt(args[2])) >> 4, cz = (Integer.parseInt(args[3])) >> 4;
            int bx = Integer.parseInt(args[2]), bz = Integer.parseInt(args[3]);
            ChunkAccess chunk = genFullChunk(s, cx, cz);
            for (int y = 75; y >= 55; y--) {
                BlockState st = chunk.getBlockState(new BlockPos(bx, y, bz));
                System.out.println("Y" + y + "\t" + net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(st.getBlock()).getPath());
            }
            return;
        }
        if (args.length > 0 && args[0].equals("treasure")) {
            // treasure server: "seed x z" -> is the chest TOUCHING AIR? Runs FULL
            // chunk gen (terrain + surface sand/gravel + water) and replicates the
            // real BuriedTreasurePiece placement, then reads the block above the chest.
            Bootstrap.STDOUT.println("READY"); Bootstrap.STDOUT.flush();
            java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(System.in));
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                if (line.equals("quit")) break;
                String[] t = line.split("\\s+");
                long s = Long.parseLong(t[0]); int x = Integer.parseInt(t[1]), z = Integer.parseInt(t[2]);
                String cls; int chestY = -1, aboveY = -1;
                try {
                    ChunkAccess chunk = genFullChunk(s, x >> 4, z >> 4);
                    chestY = chestPlacementY(chunk, x, z);
                    aboveY = chestY + 1;
                    BlockState above = chunk.getBlockState(new BlockPos(x, aboveY, z));
                    cls = !above.getFluidState().isEmpty() ? "water" : above.isAir() ? "air" : "buried";
                } catch (Throwable ex) { cls = "err"; }
                Bootstrap.STDOUT.println("T\t" + s + "\t" + x + "\t" + z
                    + "\tchestY=" + chestY + "\tabove=" + cls);
                Bootstrap.STDOUT.flush();
            }
            return;
        }
        if (args.length > 0 && args[0].equals("pieces")) {
            // "pieces <seed> <structure> <x> <z>" -> every piece's real bounding
            // box, from the GAME's own structure generator.
            //
            // Why this and not another C piece generator: cubiomes only ships
            // generators for fortress and end_city, so build/structoverlap.exe
            // answers UNKNOWN for everything else -- mansion included, which is
            // the case that prompted the whole overlap tool (a mansion sprawls
            // far past its origin, so origin-to-origin distance is meaningless
            // for it). Reimplementing mansion layout in C would be a large,
            // exactly-wrong-in-small-ways job. The real generator is already
            // here and is right by construction, and this works for EVERY
            // structure rather than one more special case.
            long seed = Long.parseLong(args[1]);
            String id = args[2];
            int tx = Integer.parseInt(args[3]), tz = Integer.parseInt(args[4]);
            Holder<Structure> st;
            try {
                st = worldgen.lookupOrThrow(Registries.STRUCTURE).getOrThrow(
                    ResourceKey.create(Registries.STRUCTURE,
                        ResourceLocation.parse(id.contains(":") ? id
                                               : "minecraft:" + id)));
            } catch (Throwable ex) {
                System.out.println("ERROR\tno such structure " + id);
                return;
            }
            MultiNoiseBiomeSource bsrc = MultiNoiseBiomeSource.createFromPreset(owParams);
            NoiseBasedChunkGenerator cg = new NoiseBasedChunkGenerator(bsrc, noiseSettings);
            RandomState rs = RandomState.create(noiseSettings.value(),
                worldgen.lookupOrThrow(Registries.NOISE), seed);
            int tcx = tx >> 4, tcz = tz >> 4;
            StructureStart found = null;
            // Searched outward rather than at the exact chunk: the caller passes
            // a block position from the finder, and a structure's start chunk is
            // not always the chunk that position lands in.
            for (int r = 0; r <= 3 && found == null; r++)
            for (int dcx = -r; dcx <= r && found == null; dcx++)
            for (int dcz = -r; dcz <= r && found == null; dcz++) {
                if (r > 0 && Math.abs(dcx) != r && Math.abs(dcz) != r) continue;
                try {
                    StructureStart start = st.value().generate(worldgen, cg, bsrc, rs,
                        templates, seed, new ChunkPos(tcx + dcx, tcz + dcz), 0,
                        OVERWORLD_HEIGHT, b -> true);
                    if (start != null && start.isValid()) found = start;
                } catch (Throwable t) { /* not here; keep looking */ }
            }
            if (found == null) {
                // NOT "no pieces". The caller must be able to tell "this engine
                // found nothing here" from "this structure has no extent", or it
                // will report a non-overlap it never actually checked.
                System.out.println("NOHIT\t" + seed + "\t" + id + "\t" + tx + "\t" + tz);
                return;
            }
            System.out.println("START\t" + seed + "\t" + id
                + "\t" + found.getChunkPos().x + "\t" + found.getChunkPos().z
                + "\tpieces=" + found.getPieces().size());
            for (net.minecraft.world.level.levelgen.structure.StructurePiece piece
                     : found.getPieces()) {
                BoundingBox b = piece.getBoundingBox();
                System.out.println("P\t" + b.minX() + "\t" + b.minY() + "\t" + b.minZ()
                    + "\t" + b.maxX() + "\t" + b.maxY() + "\t" + b.maxZ()
                    + "\t" + pieceName(piece));
            }
            return;
        }
        if (args.length > 0 && args[0].equals("height")) {
            // height <seed> <x> <z> -> real WORLD_SURFACE_WG / OCEAN_FLOOR_WG Y.
            long s = Long.parseLong(args[1]); int x = Integer.parseInt(args[2]), z = Integer.parseInt(args[3]);
            MultiNoiseBiomeSource bs = MultiNoiseBiomeSource.createFromPreset(owParams);
            NoiseBasedChunkGenerator cg = new NoiseBasedChunkGenerator(bs, noiseSettings);
            RandomState rs = RandomState.create(noiseSettings.value(), worldgen.lookupOrThrow(Registries.NOISE), s);
            int ws = cg.getBaseHeight(x, z, Heightmap.Types.WORLD_SURFACE_WG, OVERWORLD_HEIGHT, rs);
            int of = cg.getBaseHeight(x, z, Heightmap.Types.OCEAN_FLOOR_WG, OVERWORLD_HEIGHT, rs);
            System.out.println("H\t" + s + "\t" + x + "\t" + z + "\tworldSurface=" + ws + "\toceanFloor=" + of);
            return;
        }
        if (args.length > 0 && args[0].equals("heightserver")) {
            // Persistent heightmap oracle: "seed x z" per line -> the game's own
            // WORLD_SURFACE_WG and OCEAN_FLOOR_WG. This is what the C finder's
            // terrain and its 1.18+ structure placement rules are checked
            // against -- a JVM bootstrap per point would make that unaffordable.
            MultiNoiseBiomeSource bs = MultiNoiseBiomeSource.createFromPreset(owParams);
            NoiseBasedChunkGenerator cg = new NoiseBasedChunkGenerator(bs, noiseSettings);
            long cached = Long.MIN_VALUE; RandomState rs = null;
            Bootstrap.STDOUT.println("READY"); Bootstrap.STDOUT.flush();
            java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(System.in));
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                if (line.equals("quit")) break;
                String[] t = line.split("\\s+");
                long s = Long.parseLong(t[0]);
                int x = Integer.parseInt(t[1]), z = Integer.parseInt(t[2]);
                if (rs == null || cached != s) {   // RandomState is per seed and costly
                    rs = RandomState.create(noiseSettings.value(), worldgen.lookupOrThrow(Registries.NOISE), s);
                    cached = s;
                }
                int ws = cg.getBaseHeight(x, z, Heightmap.Types.WORLD_SURFACE_WG, OVERWORLD_HEIGHT, rs);
                int of = cg.getBaseHeight(x, z, Heightmap.Types.OCEAN_FLOOR_WG, OVERWORLD_HEIGHT, rs);
                Bootstrap.STDOUT.println("H\t" + s + "\t" + x + "\t" + z + "\t" + ws + "\t" + of);
                Bootstrap.STDOUT.flush();
            }
            return;
        }
        if (args.length > 0 && args[0].equals("server")) {
            // Persistent worker: "seed x z" per line -> one MAX line, then DONE.
            Bootstrap.STDOUT.println("READY"); Bootstrap.STDOUT.flush();
            java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(System.in));
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                if (line.equals("quit")) break;
                String[] t = line.split("\\s+");
                processSeed(Long.parseLong(t[0]), Integer.parseInt(t[1]), Integer.parseInt(t[2]));
                Bootstrap.STDOUT.println("DONE"); Bootstrap.STDOUT.flush();
            }
            return;
        }
        verbose = true;
        long seed = args.length > 0 ? Long.parseLong(args[0]) : 4541317274251036436L;
        int tx = args.length > 1 ? Integer.parseInt(args[1]) : 96;
        int tz = args.length > 2 ? Integer.parseInt(args[2]) : 704;
        processSeed(seed, tx, tz);
    }

    // The floor blocks BuriedTreasurePiece scans down to (the chest lands where the
    // block below is one of these -- i.e. at the BOTTOM of the sand/gravel column).
    static java.util.Set<Block> chestFloor() {
        return java.util.Set.of(net.minecraft.world.level.block.Blocks.SANDSTONE,
            net.minecraft.world.level.block.Blocks.STONE, net.minecraft.world.level.block.Blocks.ANDESITE,
            net.minecraft.world.level.block.Blocks.GRANITE, net.minecraft.world.level.block.Blocks.DIORITE);
    }

    // Replicate BuriedTreasurePiece.postProcess: from the ocean-floor surface, scan
    // DOWN until the block below is sandstone/stone/andesite/granite/diorite; the
    // chest is placed there (so on a sandy beach it sits UNDER the sand).
    static int chestPlacementY(ChunkAccess chunk, int x, int z) {
        java.util.Set<Block> floor = chestFloor();
        int y = 319;
        while (y > -64) {   // OCEAN_FLOOR_WG: topmost non-air, non-fluid block, +1
            BlockState st = chunk.getBlockState(new BlockPos(x, y, z));
            if (!st.isAir() && st.getFluidState().isEmpty()) break;
            y--;
        }
        BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos(x, y + 1, z);
        while (pos.getY() > -64) {
            if (floor.contains(chunk.getBlockState(pos.below()).getBlock())) return pos.getY();
            pos.move(0, -1, 0);
        }
        return -64;
    }

    static NoiseBasedChunkGenerator cachedGen;
    static MultiNoiseBiomeSource cachedBiomes;

    // Full headless chunk generation: terrain (fillFromNoise) + surface rules
    // (buildSurface adds sand/gravel/grass) + aquifers/water. NO ServerLevel: the
    // only StructureManager use is the beardifier's structure lookup, which we
    // stub to "no structures" (a beach chunk has none affecting terrain).
    static ChunkAccess genFullChunk(long seed, int chunkX, int chunkZ) {
        if (cachedBiomes == null) {
            cachedBiomes = MultiNoiseBiomeSource.createFromPreset(owParams);
            cachedGen = new NoiseBasedChunkGenerator(cachedBiomes, noiseSettings);
        }
        MultiNoiseBiomeSource biomeSource = cachedBiomes;
        NoiseBasedChunkGenerator chunkGen = cachedGen;
        RandomState rs = RandomState.create(noiseSettings.value(), worldgen.lookupOrThrow(Registries.NOISE), seed);
        Registry<Biome> biomeReg = worldgen.registryOrThrow(Registries.BIOME);
        StructureManager sm = new StructureManager(null, null, null) {
            public java.util.List<StructureStart> startsForStructure(ChunkPos p, java.util.function.Predicate<Structure> pred) { return java.util.List.of(); }
            public boolean hasAnyStructureAt(BlockPos p) { return false; }
            public java.util.Map<Structure, it.unimi.dsi.fastutil.longs.LongSet> getAllStructuresAt(BlockPos p) { return java.util.Map.of(); }
        };
        ChunkPos cp = new ChunkPos(chunkX, chunkZ);
        ProtoChunk chunk = new ProtoChunk(cp, UpgradeData.EMPTY, OVERWORLD_HEIGHT, biomeReg, null);
        chunkGen.createBiomes(rs, Blender.empty(), sm, chunk).join();
        chunkGen.fillFromNoise(Blender.empty(), rs, sm, chunk).join();
        WorldGenerationContext wgc = new WorldGenerationContext(chunkGen, OVERWORLD_HEIGHT);
        BiomeManager bm = new BiomeManager(
            (x, y, z) -> biomeSource.getNoiseBiome(x, y, z, rs.sampler()), seed);
        chunkGen.buildSurface(chunk, wgc, rs, sm, bm, biomeReg, Blender.empty());
        return chunk;
    }

    // The jigsaw template name of a piece, e.g. "pillager_outpost/watchtower".
    static String pieceName(net.minecraft.world.level.levelgen.structure.StructurePiece piece) {
        // Non-jigsaw pieces (mansion, monument, stronghold...) have no template
        // name. getType().toString() on those is a lambda's identity hash --
        // "StructurePieceType$$Lambda/0x...@131b016c" -- which is not a name, it
        // is noise that changes every run. The registry key is the real one.
        if (!(piece instanceof net.minecraft.world.level.levelgen.structure.PoolElementStructurePiece)) {
            try {
                ResourceLocation k = net.minecraft.core.registries.BuiltInRegistries
                    .STRUCTURE_PIECE.getKey(piece.getType());
                if (k != null) return k.getPath();
            } catch (Throwable ignored) { }
            return piece.getClass().getSimpleName();
        }
        try {
            Object el = ((net.minecraft.world.level.levelgen.structure.PoolElementStructurePiece) piece).getElement();
            if (el instanceof net.minecraft.world.level.levelgen.structure.pools.SinglePoolElement) {
                java.lang.reflect.Field f = net.minecraft.world.level.levelgen.structure.pools.SinglePoolElement.class.getDeclaredField("template");
                f.setAccessible(true);
                Object either = f.get(el);
                if (either instanceof com.mojang.datafixers.util.Either) {
                    java.util.Optional<?> left = ((com.mojang.datafixers.util.Either<?, ?>) either).left();
                    if (left.isPresent()) return left.get().toString();
                }
            }
            return el.toString();
        } catch (Throwable t) { return "?" + t; }
    }

    static void processSeed(long seed, int tx, int tz) {
        MultiNoiseBiomeSource biomeSource = MultiNoiseBiomeSource.createFromPreset(owParams);
        NoiseBasedChunkGenerator chunkGen = new NoiseBasedChunkGenerator(biomeSource, noiseSettings);
        RandomState randomState = RandomState.create(noiseSettings.value(),
            worldgen.lookupOrThrow(Registries.NOISE), seed);

        int tcx = tx >> 4, tcz = tz >> 4;
        StructureStart found = null;
        for (int dcx = -3; dcx <= 3 && found == null; dcx++)
        for (int dcz = -3; dcz <= 3 && found == null; dcz++) {
            ChunkPos cp = new ChunkPos(tcx + dcx, tcz + dcz);
            StructureStart start;
            try {
                start = outpost.value().generate(worldgen, chunkGen, biomeSource, randomState,
                    templates, seed, cp, 0, OVERWORLD_HEIGHT, b -> true);
            } catch (Throwable t) { continue; }
            if (start != null && start.isValid()) found = start;
        }
        if (found == null) { System.out.println("NOHIT\t" + seed + "\tno outpost near " + tx + "," + tz); return; }

        // For each piece, the terrain drop under its footprint. floatGap = how far
        // the piece's bottom floats above the lowest ground under it. The watchtower
        // is the tall central piece; its gap is what reads as a "stilted" tower.
        int towerGap = 0, maxGap = 0, towerX = tx, towerZ = tz;
        for (net.minecraft.world.level.levelgen.structure.StructurePiece piece : found.getPieces()) {
            BoundingBox b = piece.getBoundingBox();
            String name = pieceName(piece);
            int tMinP = Integer.MAX_VALUE;
            for (int x = b.minX(); x <= b.maxX(); x += 1)
            for (int z = b.minZ(); z <= b.maxZ(); z += 1) {
                int h = chunkGen.getBaseHeight(x, z, Heightmap.Types.WORLD_SURFACE_WG, OVERWORLD_HEIGHT, randomState);
                if (h < tMinP) tMinP = h;
            }
            int floatGap = b.minY() - tMinP;
            if (floatGap > maxGap) maxGap = floatGap;
            if (name.contains("watchtower")) {
                towerGap = floatGap; towerX = (b.minX()+b.maxX())/2; towerZ = (b.minZ()+b.maxZ())/2;
                // Tight-core float: terrain only under the tower's CENTRE (±3),
                // to avoid a cliff beside the tower inflating the gap. Also report
                // max terrain in the box (if terrain rises above the base, the
                // tower is cut into the hill = grounded, not floating).
                int coreMin = Integer.MAX_VALUE, boxMax = Integer.MIN_VALUE;
                int ccx = (b.minX()+b.maxX())/2, ccz = (b.minZ()+b.maxZ())/2;
                for (int x = b.minX(); x <= b.maxX(); x++)
                for (int z = b.minZ(); z <= b.maxZ(); z++) {
                    int h = chunkGen.getBaseHeight(x, z, Heightmap.Types.WORLD_SURFACE_WG, OVERWORLD_HEIGHT, randomState);
                    if (h > boxMax) boxMax = h;
                    if (Math.abs(x-ccx) <= 3 && Math.abs(z-ccz) <= 3 && h < coreMin) coreMin = h;
                }
                System.out.println("TOWER\t" + seed + "\tbaseY=" + b.minY()
                    + "\tcoreMinTerrain=" + coreMin + "\tboxMaxTerrain=" + boxMax
                    + "\tcoreFloat=" + (b.minY()-coreMin) + "\tboxFloat=" + floatGap);
            }
            if (verbose) System.out.println("PIECE\t" + name + "\tbox=[" + b.minX() + "," + b.minY() + "," + b.minZ()
                + " -> " + b.maxX() + "," + b.maxY() + "," + b.maxZ() + "]\tfloatGap=" + floatGap);
        }
        System.out.println("MAX\t" + seed + "\t" + towerX + "\t" + towerZ
            + "\ttowerGap=" + towerGap + "\tmaxGap=" + maxGap + "\tpieces=" + found.getPieces().size());
    }
}
