import java.util.Random;

/* Independent reference implementation for cross-validating cubiomes.
 *
 * Salts/spacing here are sourced from the Minecraft Wiki "Structure set" pages,
 * NOT read out of cubiomes -- otherwise this check would be circular. The
 * placement math comes from the vanilla algorithm using the real JDK Random,
 * including the nextInt rejection loop that cubiomes deliberately omits. */
public class XVal {

    record Cfg(String name, int salt, int regionSize, int chunkRange, boolean triangular) {}

    // spacing = regionSize; chunkRange = spacing - separation
    static final Cfg[] CFGS = {
        new Cfg("desert_pyramid", 14357617, 32, 32 - 8,  false),
        new Cfg("village",        10387312, 34, 34 - 8,  false), // 1.18+ spacing 34
        new Cfg("village_262",    10387312, 34, 34 - 8,  false),
        new Cfg("mansion",        10387319, 80, 80 - 20, true),
        new Cfg("monument",       10387313, 32, 32 - 5,  true),
        new Cfg("ancient_city",   20083232, 24, 24 - 8,  false),
    };

    static final long[] SEEDS = {
        0L, 1L, 42L, -1L, 123456789L, 8675309L, -4172144997902289642L,
        3257840388504953787L, 2151901553968352745L, -4643277814275016011L,
    };

    static int[] featureChunk(Cfg c, long worldSeed, int regX, int regZ) {
        long regionSeed = (long) regX * 341873128712L
                        + (long) regZ * 132897987541L
                        + worldSeed + c.salt();
        Random r = new Random(regionSeed);
        int x, z;
        if (c.triangular()) {
            x = (r.nextInt(c.chunkRange()) + r.nextInt(c.chunkRange())) >> 1;
            z = (r.nextInt(c.chunkRange()) + r.nextInt(c.chunkRange())) >> 1;
        } else {
            x = r.nextInt(c.chunkRange());
            z = r.nextInt(c.chunkRange());
        }
        return new int[]{x, z};
    }

    public static void main(String[] args) {
        for (Cfg c : CFGS)
            System.out.printf("CONFIG %s salt=%d regionSize=%d chunkRange=%d%n",
                    c.name(), c.salt(), c.regionSize(), c.chunkRange());

        for (Cfg c : CFGS)
            for (long seed : SEEDS)
                for (int rx = -3; rx <= 3; rx++)
                    for (int rz = -3; rz <= 3; rz++) {
                        int[] p = featureChunk(c, seed, rx, rz);
                        int bx = (int) ((long) (rx * c.regionSize() + p[0]) << 4);
                        int bz = (int) ((long) (rz * c.regionSize() + p[1]) << 4);
                        System.out.printf("POS %s %d %d %d %d %d%n",
                                c.name(), seed, rx, rz, bx, bz);
                    }
    }
}
