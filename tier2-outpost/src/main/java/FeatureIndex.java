import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.LayeredRegistryAccess;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.RegistryDataLoader;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeGenerationSettings;
import net.minecraft.world.level.biome.FeatureSorter;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.levelgen.placement.PlacedFeature;

import java.util.List;

// FeatureIndex -- dump the index every placed feature is seeded with.
//
//   java FeatureIndex [substring]
//
// Decoration RNG is seeded as populationSeed + index + 10000 * step, and getting
// that index wrong still produces plausible output, just in the wrong places.
// It is NOT a count through a biome's own feature list: ChunkGenerator asks
// FeatureSorter for a topologically sorted list of every placed feature that any
// biome in the world can contribute to a step, and indexes into THAT.
//
// Rather than reimplement the sort and hope, this calls the game's own sorter
// with the same arguments ChunkGenerator passes -- including the biome list in
// biomeSource.possibleBiomes() order, which the result depends on. Whatever it
// prints is by construction what the real game uses.
public class FeatureIndex {

    public static void main(String[] args) throws Exception {
        String filter = args.length > 0 ? args[0] : null;

        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        PackRepository repo = ServerPacksSource.createVanillaTrustedRepository();
        repo.reload();
        repo.setSelected(repo.getAvailableIds());
        List<net.minecraft.server.packs.PackResources> packs = repo.openAllSelected();
        MultiPackResourceManager rm = new MultiPackResourceManager(PackType.SERVER_DATA, packs);
        LayeredRegistryAccess<RegistryLayer> layered = RegistryLayer.createRegistryAccess();
        RegistryAccess.Frozen wg = RegistryDataLoader.load(
            rm, layered.getAccessForLoading(RegistryLayer.WORLDGEN),
            RegistryDataLoader.WORLDGEN_REGISTRIES);
        RegistryAccess.Frozen worldgen = layered.replaceFrom(RegistryLayer.WORLDGEN, wg).compositeAccess();

        Holder<MultiNoiseBiomeSourceParameterList> owParams =
            worldgen.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST)
                    .getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);
        MultiNoiseBiomeSource biomeSource = MultiNoiseBiomeSource.createFromPreset(owParams);

        // Exactly ChunkGenerator's own call, arguments and all.
        List<FeatureSorter.StepFeatureData> perStep = FeatureSorter.buildFeaturesPerStep(
            List.copyOf(biomeSource.possibleBiomes()),
            holder -> ((Biome) holder.value()).getGenerationSettings().features(),
            true);

        // Names, so the output is readable: PlacedFeature has no id of its own,
        // only its registry entry does.
        Registry<PlacedFeature> reg = worldgen.registryOrThrow(Registries.PLACED_FEATURE);

        System.out.println("# overworld, " + perStep.size() + " generation steps");
        System.out.println("# step\tindex\tfeature");
        for (int step = 0; step < perStep.size(); step++) {
            List<PlacedFeature> feats = perStep.get(step).features();
            for (PlacedFeature pf : feats) {
                int index = perStep.get(step).indexMapping().applyAsInt(pf);
                ResourceLocation id = reg.getKey(pf);
                String name = id == null ? "<unregistered>" : id.toString();
                if (filter != null && !name.contains(filter)) continue;
                System.out.println(step + "\t" + index + "\t" + name);
            }
        }
    }
}
