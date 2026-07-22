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

    public static void main(String[] args) throws Exception {
        long seed = args.length > 0 ? Long.parseLong(args[0]) : 42L;
        int radius = args.length > 1 ? Integer.parseInt(args[1]) : 2000;
        int threshold = args.length > 2 ? Integer.parseInt(args[2]) : 1;

        Bootstrap.bootStrap();
        net.minecraft.core.RegistryAccess registries = net.minecraft.core.RegistryAccess.builtin();

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
        StructureManager structureManager = new StructureManager(rm, lsa, DataFixers.getDataFixer());

        Registry<NoiseGeneratorSettings> noiseReg = BuiltinRegistries.NOISE_GENERATOR_SETTINGS;
        NoiseGeneratorSettings overworld = noiseReg.getOrThrow(NoiseGeneratorSettings.OVERWORLD);
        StructureFeatureConfiguration villageCfg = overworld.structureSettings().getConfig(StructureFeature.VILLAGE);

        OverworldBiomeSource biomeSource = new OverworldBiomeSource(seed, false, false, BuiltinRegistries.BIOME);
        NoiseBasedChunkGenerator chunkGen = new NoiseBasedChunkGenerator(biomeSource, seed, () -> overworld);

        Registry<ConfiguredStructureFeature<?, ?>> confReg = BuiltinRegistries.CONFIGURED_STRUCTURE_FEATURE;

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
                StructureStart<?> start = conf.generate(registries, chunkGen, biomeSource, structureManager, seed, potential, biome, 0, villageCfg);
                if (start == null || !start.isValid()) continue;
                Map<String, Integer> smiths = new TreeMap<>();
                int total = 0;
                for (StructurePiece piece : start.getPieces()) {
                    if (!(piece instanceof PoolElementStructurePiece)) continue;
                    String name = templateName(((PoolElementStructurePiece) piece).getElement());
                    if (name == null) continue;
                    String s = shortName(name);
                    // smith buildings across village types: *_tool_smith, *_weapon_smith,
                    // *_weaponsmith, *_armorer_house, *_armorer
                    if (s.contains("tool_smith") || s.contains("toolsmith")
                        || s.contains("weapon_smith") || s.contains("weaponsmith")
                        || s.contains("armorer")) {
                        smiths.merge(s, 1, Integer::sum);
                        total++;
                    }
                }
                if (total >= threshold) {
                    int wx = cx * 16 + 8, wz = cz * 16 + 8;
                    System.out.println(seed + "\tx=" + wx + "\tz=" + wz + "\tbiome=" + biomeName(biome)
                        + "\tsmiths=" + total + "\t" + smiths);
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
