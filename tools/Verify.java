import java.util.Random;

/* Independently verify a seed the C searcher reported, using the JDK-validated
 * reference implementation rather than cubiomes. Geometry only -- biome
 * viability can't be cross-checked this way. */
public class Verify {

    static int[] mansionAt(long worldSeed, int regX, int regZ) {
        long rs = (long) regX * 341873128712L + (long) regZ * 132897987541L
                + worldSeed + 10387319L;            // mansion salt, wiki-sourced
        Random r = new Random(rs);
        int cr = 80 - 20;                            // spacing 80, separation 20
        int x = (r.nextInt(cr) + r.nextInt(cr)) >> 1;  // triangular
        int z = (r.nextInt(cr) + r.nextInt(cr)) >> 1;
        return new int[]{ (regX * 80 + x) << 4, (regZ * 80 + z) << 4 };
    }

    static int[] villageAt(long worldSeed, int regX, int regZ) {
        long rs = (long) regX * 341873128712L + (long) regZ * 132897987541L
                + worldSeed + 10387312L;            // village salt
        Random r = new Random(rs);
        int cr = 34 - 8;                             // 1.18+ spacing 34
        int x = r.nextInt(cr);
        int z = r.nextInt(cr);
        return new int[]{ (regX * 34 + x) << 4, (regZ * 34 + z) << 4 };
    }

    public static void main(String[] args) {
        long seed = Long.parseLong(args[0]);
        int expMx = Integer.parseInt(args[1]), expMz = Integer.parseInt(args[2]);
        int expVx = Integer.parseInt(args[3]), expVz = Integer.parseInt(args[4]);

        System.out.println("seed " + seed);
        System.out.println("  structure seed (low 48) = " + (seed & ((1L << 48) - 1)));

        boolean mOk = false, vOk = false;
        for (int rx = -1; rx <= 1 && !mOk; rx++)
            for (int rz = -1; rz <= 1 && !mOk; rz++) {
                int[] p = mansionAt(seed, rx, rz);
                if (p[0] == expMx && p[1] == expMz) {
                    System.out.printf("  mansion  region(%d,%d) -> x=%d z=%d  MATCH%n", rx, rz, p[0], p[1]);
                    mOk = true;
                }
            }
        for (int rx = -2; rx <= 2 && !vOk; rx++)
            for (int rz = -2; rz <= 2 && !vOk; rz++) {
                int[] p = villageAt(seed, rx, rz);
                if (p[0] == expVx && p[1] == expVz) {
                    System.out.printf("  village  region(%d,%d) -> x=%d z=%d  MATCH%n", rx, rz, p[0], p[1]);
                    vOk = true;
                }
            }
        if (!mOk) System.out.printf("  mansion  EXPECTED x=%d z=%d  NOT FOUND%n", expMx, expMz);
        if (!vOk) System.out.printf("  village  EXPECTED x=%d z=%d  NOT FOUND%n", expVx, expVz);
        System.out.println(mOk && vOk ? "  => VERIFIED" : "  => FAILED");
        if (!(mOk && vOk)) System.exit(1);
    }
}
