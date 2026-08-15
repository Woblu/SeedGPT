import com.mojang.datafixers.util.Either;
import net.minecraft.SharedConstants;
import net.minecraft.core.Registry;
import net.minecraft.data.BuiltinRegistries;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.Pack;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.SimpleReloadableResourceManager;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.OverworldBiomeSource;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.feature.ConfiguredStructureFeature;
import net.minecraft.world.level.levelgen.feature.StructureFeature;
import net.minecraft.world.level.levelgen.feature.configurations.StructureFeatureConfiguration;
import net.minecraft.world.level.levelgen.feature.structures.SinglePoolElement;
import net.minecraft.world.level.levelgen.feature.structures.StructurePoolElement;
import net.minecraft.world.level.levelgen.structure.PoolElementStructurePiece;
import net.minecraft.world.level.levelgen.structure.StructurePiece;
import net.minecraft.world.level.levelgen.structure.StructureStart;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureManager;

import java.lang.reflect.Field;
import java.util.*;

// Headless Minecraft 1.16.1 worldgen: given a seed, generate villages near a
// point using the REAL generator (terrain + jigsaw) and count building types.
public class VillageWorldgen {

    // One-time worldgen setup shared across seeds (the expensive part).
    static net.minecraft.core.RegistryAccess registries;
    static StructureManager structureManager;
    static NoiseGeneratorSettings overworld;
    static StructureFeatureConfiguration villageCfg;
    static Registry<ConfiguredStructureFeature<?, ?>> confReg;

    public static void main(String[] args) throws Exception {
        Bootstrap.bootStrap();
        registries = net.minecraft.core.RegistryAccess.builtin();

        // Vanilla data pack -> resource manager -> StructureManager (loads templates).
        PackRepository packRepo = new PackRepository(Pack::new, new ServerPacksSource());
        packRepo.reload();
        packRepo.setSelected(packRepo.getAvailableIds());
        SimpleReloadableResourceManager rm = new SimpleReloadableResourceManager(PackType.SERVER_DATA);
        for (Pack p : packRepo.getSelectedPacks()) rm.add(p.open());
        java.nio.file.Path tmp = java.nio.file.Files.createTempDirectory("vwg");
        net.minecraft.world.level.storage.LevelStorageSource lss =
            net.minecraft.world.level.storage.LevelStorageSource.createDefault(tmp);
        net.minecraft.world.level.storage.LevelStorageSource.LevelStorageAccess lsa = lss.createAccess("w");
        structureManager = new StructureManager(rm, lsa, DataFixers.getDataFixer());
        overworld = BuiltinRegistries.NOISE_GENERATOR_SETTINGS.getOrThrow(NoiseGeneratorSettings.OVERWORLD);
        villageCfg = overworld.structureSettings().getConfig(StructureFeature.VILLAGE);
        confReg = BuiltinRegistries.CONFIGURED_STRUCTURE_FEATURE;

        if (args.length > 0 && args[0].equals("column")) {
            // "column <seed> <x> <z> [y0 y1]" -- the NOISE terrain column, 1.16.5.
            //
            // getBaseColumn needs no WorldGenRegion, no ServerLevel and no
            // StructureFeatureManager, which is what blocked every previous
            // attempt at block-level 1.16 terrain. It returns base stone/water/
            // air only: no surface rules and NO BEDROCK, both of which come
            // later. That is a feature here, not a gap -- it shows the shape of
            // the column the buried treasure scan actually falls down.
            long seed = Long.parseLong(args[1]);
            int x = Integer.parseInt(args[2]), z = Integer.parseInt(args[3]);
            int y0 = args.length > 4 ? Integer.parseInt(args[4]) : 0;
            int y1 = args.length > 5 ? Integer.parseInt(args[5]) : 70;
            OverworldBiomeSource bs = new OverworldBiomeSource(
                seed, false, false, BuiltinRegistries.BIOME);
            NoiseBasedChunkGenerator cg = new NoiseBasedChunkGenerator(
                bs, seed, () -> overworld);
            net.minecraft.world.level.BlockGetter col = cg.getBaseColumn(x, z);
            System.out.println("seed " + seed + " column (" + x + "," + z + ")"
                + "   OCEAN_FLOOR_WG=" + cg.getBaseHeight(x, z,
                      net.minecraft.world.level.levelgen.Heightmap.Types.OCEAN_FLOOR_WG)
                + "   WORLD_SURFACE_WG=" + cg.getBaseHeight(x, z,
                      net.minecraft.world.level.levelgen.Heightmap.Types.WORLD_SURFACE_WG));
            System.out.println("NOTE: noise terrain only -- bedrock (y0-4) and "
                + "surface rules are applied later and are NOT shown here.");
            for (int y = y1; y >= y0; y--) {
                net.minecraft.world.level.block.state.BlockState st =
                    col.getBlockState(new net.minecraft.core.BlockPos(x, y, z));
                System.out.println(y + "	" + Registry.BLOCK.getKey(st.getBlock()));
            }
            return;
        }
        if (args.length > 0 && args[0].equals("carvediff")) {
            // "carvediff <seed> <x> <z>" -- does CARVING change where a buried
            // treasure lands and what it is cased in?
            //
            // This exists to test one hypothesis on the tier-2 1.21.1 path,
            // which disagrees with a real server on ~7% of faces. Its
            // genFullChunk runs createBiomes + fillFromNoise + buildSurface and
            // never applyCarvers, while a real server chunk is carved between
            // SURFACE and FEATURES. 1.21.1's applyCarvers takes a WorldGenRegion
            // -- a concrete class the dynamic-proxy trick cannot supply -- so
            // the hypothesis is untestable there. 1.16.5's takes a BiomeManager,
            // so it IS testable here.
            //
            // Different version, so this is evidence about the MECHANISM, not a
            // measurement of the 1.21.1 error. If carving moves nothing in 1.16
            // the hypothesis is weak; if it moves chests and faces, it is the
            // best remaining explanation and worth the cost of reaching it.
            long sd = Long.parseLong(args[1]);
            int x = Integer.parseInt(args[2]), z = Integer.parseInt(args[3]);
            OverworldBiomeSource bs = new OverworldBiomeSource(
                sd, false, false, BuiltinRegistries.BIOME);
            NoiseBasedChunkGenerator cg = new NoiseBasedChunkGenerator(bs, sd, () -> overworld);
            ChunkPos cp = new ChunkPos(x >> 4, z >> 4);
            net.minecraft.world.level.chunk.ProtoChunk[] both =
                new net.minecraft.world.level.chunk.ProtoChunk[2];
            for (int pass = 0; pass < 2; pass++) {
                net.minecraft.world.level.chunk.ProtoChunk ch =
                    new net.minecraft.world.level.chunk.ProtoChunk(
                        cp, net.minecraft.world.level.chunk.UpgradeData.EMPTY);
                net.minecraft.core.BlockPos.MutableBlockPos mp =
                    new net.minecraft.core.BlockPos.MutableBlockPos();
                for (int bx = 0; bx < 16; bx++)
                for (int bz = 0; bz < 16; bz++) {
                    int wx = cp.getMinBlockX() + bx, wz = cp.getMinBlockZ() + bz;
                    net.minecraft.world.level.BlockGetter col = cg.getBaseColumn(wx, wz);
                    for (int y = 0; y < 256; y++) {
                        net.minecraft.world.level.block.state.BlockState st =
                            col.getBlockState(mp.set(wx, y, wz));
                        if (!st.isAir()) ch.setBlockState(mp.set(wx, y, wz), st, false);
                    }
                }
                if (pass == 1) {
                    net.minecraft.world.level.biome.BiomeManager bm =
                        new net.minecraft.world.level.biome.BiomeManager(bs, sd,
                            net.minecraft.world.level.biome.FuzzyOffsetBiomeZoomer.INSTANCE);
                    cg.applyCarvers(sd, bm, ch,
                        net.minecraft.world.level.levelgen.GenerationStep.Carving.AIR);
                    cg.applyCarvers(sd, bm, ch,
                        net.minecraft.world.level.levelgen.GenerationStep.Carving.LIQUID);
                }
                both[pass] = ch;
            }
            String[] label = {"uncarved", "carved"};
            for (int pass = 0; pass < 2; pass++) {
                net.minecraft.world.level.chunk.ProtoChunk ch = both[pass];
                int y = 255;
                while (y > 0) {
                    net.minecraft.world.level.block.state.BlockState st =
                        ch.getBlockState(new net.minecraft.core.BlockPos(x, y, z));
                    if (!st.isAir() && st.getFluidState().isEmpty()) break;
                    y--;
                }
                // BuriedTreasurePiece's own descent, 1.16.5 rule.
                int cy = y + 1;
                while (cy > 0) {
                    net.minecraft.world.level.block.Block b = ch.getBlockState(
                        new net.minecraft.core.BlockPos(x, cy - 1, z)).getBlock();
                    if (b == net.minecraft.world.level.block.Blocks.SANDSTONE
                     || b == net.minecraft.world.level.block.Blocks.STONE
                     || b == net.minecraft.world.level.block.Blocks.ANDESITE
                     || b == net.minecraft.world.level.block.Blocks.GRANITE
                     || b == net.minecraft.world.level.block.Blocks.DIORITE) break;
                    cy--;
                }
                StringBuilder faces = new StringBuilder();
                int[][] f = {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
                for (int[] d : f)
                    faces.append(',').append(Registry.BLOCK.getKey(ch.getBlockState(
                        new net.minecraft.core.BlockPos(x + d[0], cy + d[1], z + d[2])).getBlock()));
                System.out.println(label[pass] + "\tchestY=" + cy
                    + "\tat=" + Registry.BLOCK.getKey(ch.getBlockState(
                          new net.minecraft.core.BlockPos(x, cy, z)).getBlock())
                    + "\tfaces=" + faces.substring(1));
            }
            return;
        }
        if (args.length > 0 && args[0].equals("carveserver")) {
            // Persistent worker: "seed x z" -> how far down the column is carved
            // open, and what is at the bottom. One JVM for a whole hunt.
            //
            // This is the buried-treasure-in-bedrock search. A chest is encased
            // in bedrock only when it lands INSIDE the bedrock band (y1-4),
            // which needs the placement scan to fall all the way down an open
            // column -- bedrock is not a stopping block, so the chest falls
            // THROUGH the band until the block below is finally stone, and the
            // block at its own position is then bedrock, which becomes the fill
            // and is written into every air/liquid face.
            //
            // The rare part is the column, not the casing: it needs a ravine
            // reaching from the ocean floor into the bedrock band at exactly the
            // treasure's (9,9). So that is what this screens for.
            System.out.println("READY");
            System.out.flush();
            java.io.BufferedReader in = new java.io.BufferedReader(
                new java.io.InputStreamReader(System.in));
            String line;
            long cachedSeed = Long.MIN_VALUE;
            OverworldBiomeSource bs = null;
            NoiseBasedChunkGenerator cg = null;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                if (line.equals("quit")) break;
                String[] t = line.split("\\s+");
                long sd = Long.parseLong(t[0]);
                int x = Integer.parseInt(t[1]), z = Integer.parseInt(t[2]);
                StringBuilder sb = new StringBuilder();
                try {
                    if (cg == null || cachedSeed != sd) {
                        bs = new OverworldBiomeSource(sd, false, false,
                                                      BuiltinRegistries.BIOME);
                        cg = new NoiseBasedChunkGenerator(bs, sd, () -> overworld);
                        cachedSeed = sd;
                    }
                    ChunkPos cp = new ChunkPos(x >> 4, z >> 4);
                    net.minecraft.world.level.chunk.ProtoChunk chunk =
                        new net.minecraft.world.level.chunk.ProtoChunk(
                            cp, net.minecraft.world.level.chunk.UpgradeData.EMPTY);
                    net.minecraft.core.BlockPos.MutableBlockPos mp =
                        new net.minecraft.core.BlockPos.MutableBlockPos();
                    for (int bx = 0; bx < 16; bx++)
                    for (int bz = 0; bz < 16; bz++) {
                        int wx = cp.getMinBlockX() + bx, wz = cp.getMinBlockZ() + bz;
                        net.minecraft.world.level.BlockGetter c = cg.getBaseColumn(wx, wz);
                        for (int y = 0; y < 256; y++) {
                            net.minecraft.world.level.block.state.BlockState st =
                                c.getBlockState(mp.set(wx, y, wz));
                            if (!st.isAir()) chunk.setBlockState(mp.set(wx, y, wz), st, false);
                        }
                    }
                    net.minecraft.world.level.biome.BiomeManager bm =
                        new net.minecraft.world.level.biome.BiomeManager(bs, sd,
                            net.minecraft.world.level.biome.FuzzyOffsetBiomeZoomer.INSTANCE);
                    cg.applyCarvers(sd, bm, chunk,
                        net.minecraft.world.level.levelgen.GenerationStep.Carving.AIR);
                    cg.applyCarvers(sd, bm, chunk,
                        net.minecraft.world.level.levelgen.GenerationStep.Carving.LIQUID);
                    // Walk down from the ocean floor exactly as the placement
                    // scan does, but only through what carving can produce:
                    // stop at the first block that is neither air nor liquid.
                    int floor = cg.getBaseHeight(x, z,
                        net.minecraft.world.level.levelgen.Heightmap.Types.OCEAN_FLOOR_WG);
                    int y = Math.min(floor, 255), openTo = y;
                    while (y > 0) {
                        net.minecraft.world.level.block.state.BlockState st =
                            chunk.getBlockState(new net.minecraft.core.BlockPos(x, y, z));
                        boolean passable = st.isAir() || !st.getFluidState().isEmpty();
                        if (!passable) break;
                        openTo = y;
                        y--;
                    }
                    net.minecraft.world.level.block.state.BlockState bottom =
                        chunk.getBlockState(new net.minecraft.core.BlockPos(x, y, z));
                    sb.append("C\t").append(sd).append('\t').append(x).append('\t').append(z)
                      .append("\toceanFloor=").append(floor)
                      .append("\topenTo=").append(openTo)
                      .append("\tbottom=").append(Registry.BLOCK.getKey(bottom.getBlock()))
                      .append("\tbottomY=").append(y);
                } catch (Throwable ex) {
                    sb.setLength(0);
                    sb.append("C\t").append(sd).append('\t').append(x).append('\t').append(z)
                      .append("\terr=").append(ex);
                }
                System.out.println(sb);
                System.out.flush();
            }
            return;
        }
        if (args.length > 0 && args[0].equals("carved")) {
            // "carved <seed> <x> <z> [y0 y1]" -- terrain WITH caves and ravines.
            //
            // getBaseColumn stops before the carvers, and for a buried treasure
            // that is the whole story: the placement scan falls down an OPEN
            // column, and only carving makes one. So the noise terrain is built
            // into a ProtoChunk by hand and applyCarvers is run over it, which
            // needs a BiomeManager and a ChunkAccess but -- unlike fillFromNoise
            // -- no LevelAccessor and no StructureFeatureManager. That is what
            // makes this reachable headlessly at all.
            //
            // Still absent: surface rules, and BEDROCK (y0-4), which is applied
            // afterwards and OVERWRITES whatever the carvers left. So an open
            // column reaching y<=4 here means the treasure scan can descend into
            // the bedrock band; what it lands on there is decided by bedrock.
            long seed = Long.parseLong(args[1]);
            int x = Integer.parseInt(args[2]), z = Integer.parseInt(args[3]);
            int y0 = args.length > 4 ? Integer.parseInt(args[4]) : 0;
            int y1 = args.length > 5 ? Integer.parseInt(args[5]) : 70;
            OverworldBiomeSource bs = new OverworldBiomeSource(
                seed, false, false, BuiltinRegistries.BIOME);
            NoiseBasedChunkGenerator cg = new NoiseBasedChunkGenerator(
                bs, seed, () -> overworld);
            ChunkPos cp = new ChunkPos(x >> 4, z >> 4);
            net.minecraft.world.level.chunk.ProtoChunk chunk =
                new net.minecraft.world.level.chunk.ProtoChunk(
                    cp, net.minecraft.world.level.chunk.UpgradeData.EMPTY);
            net.minecraft.core.BlockPos.MutableBlockPos mp =
                new net.minecraft.core.BlockPos.MutableBlockPos();
            for (int bx = 0; bx < 16; bx++) {
                for (int bz = 0; bz < 16; bz++) {
                    int wx = cp.getMinBlockX() + bx, wz = cp.getMinBlockZ() + bz;
                    net.minecraft.world.level.BlockGetter c = cg.getBaseColumn(wx, wz);
                    for (int y = 0; y < 256; y++) {
                        net.minecraft.world.level.block.state.BlockState st =
                            c.getBlockState(mp.set(wx, y, wz));
                        if (!st.isAir()) chunk.setBlockState(mp.set(wx, y, wz), st, false);
                    }
                }
            }
            net.minecraft.world.level.biome.BiomeManager bm =
                new net.minecraft.world.level.biome.BiomeManager(bs, seed,
                    net.minecraft.world.level.biome.FuzzyOffsetBiomeZoomer.INSTANCE);
            cg.applyCarvers(seed, bm, chunk, net.minecraft.world.level.levelgen.GenerationStep.Carving.AIR);
            cg.applyCarvers(seed, bm, chunk, net.minecraft.world.level.levelgen.GenerationStep.Carving.LIQUID);
            System.out.println("seed " + seed + " CARVED column (" + x + "," + z + ")"
                + "   OCEAN_FLOOR_WG(noise)=" + cg.getBaseHeight(x, z,
                      net.minecraft.world.level.levelgen.Heightmap.Types.OCEAN_FLOOR_WG));
            System.out.println("bedrock (y0-4) and surface rules are applied "
                + "AFTER this and are NOT shown.");
            int openLow = -1;
            for (int y = y1; y >= y0; y--) {
                net.minecraft.world.level.block.state.BlockState st =
                    chunk.getBlockState(new net.minecraft.core.BlockPos(x, y, z));
                boolean air = st.isAir();
                if (air) openLow = y;
                System.out.println(y + "	" + Registry.BLOCK.getKey(st.getBlock())
                    + (air ? "	<== CARVED OPEN" : ""));
            }
            System.out.println("lowest carved-open y in range: " + openLow);
            return;
        }
        if (args.length > 0 && args[0].equals("server")) {
            // Persistent worker: read "seed radius threshold" lines from stdin,
            // emit matching villages, then a "seed DONE" line. Amortises setup.
            Bootstrap.STDOUT.println("READY");
            Bootstrap.STDOUT.flush();
            java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(System.in));
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                if (line.equals("quit")) break;
                String[] t = line.split("\\s+");
                long seed = Long.parseLong(t[0]);
                int radius = t.length > 1 ? Integer.parseInt(t[1]) : 1200;
                int threshold = t.length > 2 ? Integer.parseInt(t[2]) : 1;
                String key = t.length > 3 ? t[3] : "smith";
                processSeed(seed, radius, threshold, key);
                Bootstrap.STDOUT.println(seed + "\tDONE");
                Bootstrap.STDOUT.flush();
            }
            return;
        }

        long seed = args.length > 0 ? Long.parseLong(args[0]) : 42L;
        int radius = args.length > 1 ? Integer.parseInt(args[1]) : 2000;
        int threshold = args.length > 2 ? Integer.parseInt(args[2]) : 1;
        String key = args.length > 3 ? args[3] : "smith";
        processSeed(seed, radius, threshold, key);
    }

    // Normalise a piece template name to a canonical BUILDING type, or null for
    // streets/decorations/mobs/small houses we don't count as "buildings".
    // Names vary by village type (taiga_weaponsmith vs snowy_weapon_smith), so
    // match on substrings.
    static String canon(String s) {
        if (s.contains("tool_smith") || s.contains("toolsmith")) return "toolsmith";
        if (s.contains("weapon_smith") || s.contains("weaponsmith")) return "weaponsmith";
        if (s.contains("armorer")) return "armorer";
        if (s.contains("library")) return "library";
        if (s.contains("cartographer")) return "cartographer";
        if (s.contains("mason")) return "mason";
        if (s.contains("fletcher")) return "fletcher";
        if (s.contains("butcher")) return "butcher";
        if (s.contains("shepherd")) return "shepherd";
        if (s.contains("fisher")) return "fisher";
        if (s.contains("tannery")) return "tannery";
        if (s.contains("temple")) return "temple";
        if (s.contains("stable")) return "stable";
        if (s.contains("farm")) return "farm";
        if (s.contains("animal_pen")) return "animal_pen";
        return null;
    }

    static void processSeed(long seed, int radius, int threshold, String key) {
        OverworldBiomeSource biomeSource = new OverworldBiomeSource(seed, false, false, BuiltinRegistries.BIOME);
        NoiseBasedChunkGenerator chunkGen = new NoiseBasedChunkGenerator(biomeSource, seed, () -> overworld);
        int rChunks = radius / 16;
        WorldgenRandom rand = new WorldgenRandom();
        for (int cx = -rChunks; cx <= rChunks; cx++) {
            for (int cz = -rChunks; cz <= rChunks; cz++) {
                ChunkPos potential = StructureFeature.VILLAGE.getPotentialFeatureChunk(villageCfg, seed, rand, cx, cz);
                if (potential.x != cx || potential.z != cz) continue;
                int bx = cx * 16 + 9, bz = cz * 16 + 9;
                Biome biome = biomeSource.getNoiseBiome(bx >> 2, 0, bz >> 2);
                ConfiguredStructureFeature<?, ?> conf = pickVillage(confReg, biome);
                if (conf == null) continue;
                StructureStart<?> start;
                try {
                    start = conf.generate(registries, chunkGen, biomeSource, structureManager, seed, potential, biome, 0, villageCfg);
                } catch (Throwable t) { continue; }
                if (start == null || !start.isValid()) continue;
                Map<String, Integer> buildings = new TreeMap<>();
                for (StructurePiece piece : start.getPieces()) {
                    if (!(piece instanceof PoolElementStructurePiece)) continue;
                    String name = templateName(((PoolElementStructurePiece) piece).getElement());
                    if (name == null) continue;
                    String c = canon(shortName(name));
                    if (c != null) buildings.merge(c, 1, Integer::sum);
                }
                // match count for the requested key ("smith" = the three smiths)
                int match = key.equals("smith")
                    ? buildings.getOrDefault("toolsmith", 0)
                        + buildings.getOrDefault("weaponsmith", 0)
                        + buildings.getOrDefault("armorer", 0)
                    : buildings.getOrDefault(key, 0);
                if (match >= threshold) {
                    int wx = cx * 16 + 8, wz = cz * 16 + 8;
                    Bootstrap.STDOUT.println("HIT\t" + seed + "\t" + wx + "\t" + wz + "\t" + biomeName(biome)
                        + "\t" + match + "\t" + buildings);
                    Bootstrap.STDOUT.flush();
                }
            }
        }
    }

    static ConfiguredStructureFeature<?, ?> pickVillage(Registry<ConfiguredStructureFeature<?, ?>> reg, Biome biome) {
        String cat = biome.getBiomeCategory().getName();
        String want;
        switch (cat) {
            case "desert": want = "village_desert"; break;
            case "savanna": want = "village_savanna"; break;
            case "icy": want = "village_snowy"; break;
            case "taiga": want = "village_taiga"; break;
            default: want = "village_plains"; break;
        }
        for (Map.Entry<net.minecraft.resources.ResourceKey<ConfiguredStructureFeature<?, ?>>, ConfiguredStructureFeature<?, ?>> e : reg.entrySet()) {
            if (e.getKey().location().getPath().equals(want)) return e.getValue();
        }
        return null;
    }

    static String templateName(StructurePoolElement el) {
        if (!(el instanceof SinglePoolElement)) return el.toString();
        try {
            Field f = SinglePoolElement.class.getDeclaredField("template");
            f.setAccessible(true);
            Object either = f.get(el);
            if (either instanceof Either) {
                Optional<?> left = ((Either<?, ?>) either).left();
                if (left.isPresent()) return left.get().toString();
            }
        } catch (Throwable t) { return el.toString(); }
        return el.toString();
    }

    static String shortName(String templ) {
        int i = templ.lastIndexOf('/');
        String s = i >= 0 ? templ.substring(i + 1) : templ;
        return s.replaceAll("_[0-9]+$", "");
    }

    static String biomeName(Biome b) {
        ResourceLocation rl = BuiltinRegistries.BIOME.getKey(b);
        return rl == null ? "?" : rl.getPath();
    }
}
