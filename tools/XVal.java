import java.util.Random;

/* Independent reference implementation for cross-validating cubiomes.
 *
 * Salts/spacing here are sourced from the Minecraft Wiki "Structure set" pages,
 * NOT read out of cubiomes -- otherwise this check would be circular. The
 * placement math comes from the vanilla algorithm using the real JDK Random,
 * including the nextInt rejection loop that cubiomes deliberately omits.
 *
 * Covers all three dimensions. Beyond raw positions this also replicates the
 * per-site GATES, which are where the dimensions genuinely differ:
 *   - fortress (1.18+): no gate, always generates where a bastion doesn't
 *   - bastion  (1.18+): chunkGenerateRnd -> nextInt(5) >= 2
 *   - end city:         triangular spread, then >= 1008 blocks from origin
 */
public class XVal {

    static final long MULT = 0x5DEECE66DL;

    enum Gate { NONE, BASTION, END_CITY }

    record Cfg(String name, int salt, int regionSize, int chunkRange,
               boolean triangular, Gate gate) {}

    // spacing = regionSize; chunkRange = spacing - separation
    static final Cfg[] CFGS = {
        new Cfg("desert_pyramid", 14357617, 32, 32 - 8,  false, Gate.NONE),
        new Cfg("village",        10387312, 34, 34 - 8,  false, Gate.NONE), // 1.18+ spacing 34
        new Cfg("village_262",    10387312, 34, 34 - 8,  false, Gate.NONE),
        new Cfg("mansion",        10387319, 80, 80 - 20, true,  Gate.NONE),
        new Cfg("monument",       10387313, 32, 32 - 5,  true,  Gate.NONE),
        new Cfg("ancient_city",   20083232, 24, 24 - 8,  false, Gate.NONE),
        // nether: fortress and bastion share salt AND geometry; on 1.18+ both
        // resolve position via the plain linear spread, then a per-chunk
        // nextInt(5) decides which of the two actually generates there.
        new Cfg("fortress",       30084232, 27, 27 - 4,  false, Gate.NONE),
        new Cfg("bastion",        30084232, 27, 27 - 4,  false, Gate.BASTION),
        // end: triangular, plus a >=1008-block exclusion around the origin
        new Cfg("end_city",       10387313, 20, 20 - 11, true,  Gate.END_CITY),
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

    /* cubiomes chunkGenerateRnd:
     *   setSeed(rnd, worldSeed)
     *   rnd = (nextLong(rnd)*chunkX) ^ (nextLong(rnd)*chunkZ) ^ worldSeed
     *   setSeed(rnd, rnd)
     * java.util.Random's constructor performs the same scramble, and its
     * nextLong is the same two-next(32) composition, so this maps directly. */
    static Random chunkGenerateRnd(long worldSeed, int chunkX, int chunkZ) {
        Random r = new Random(worldSeed);
        long rnd = (r.nextLong() * chunkX) ^ (r.nextLong() * chunkZ) ^ worldSeed;
        return new Random(rnd);
    }

    static boolean gatePasses(Cfg c, long worldSeed, int bx, int bz) {
        switch (c.gate()) {
            case BASTION:
                return chunkGenerateRnd(worldSeed, bx >> 4, bz >> 4).nextInt(5) >= 2;
            case END_CITY:
                return (long) bx * bx + (long) bz * bz >= 1008L * 1008L;
            default:
                return true;
        }
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
                        if (!gatePasses(c, seed, bx, bz)) continue;
                        System.out.printf("POS %s %d %d %d %d %d%n",
                                c.name(), seed, rx, rz, bx, bz);
                    }
    }
}
